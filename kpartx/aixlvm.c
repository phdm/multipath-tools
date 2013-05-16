/*
 * Copyright (c) 2012-2013 Philippe De Muyter <phdm@macq.eu>
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "byteorder.h"
#include "kpartx.h"

typedef int __be32;
typedef short __be16;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

struct lvm_rec {
	char lvm_id[4]; /* "_LVM" */
	char reserved4[16];
	__be32 lvmarea_len;
	__be32 vgda_len;
	__be32 vgda_psn[2];
	char reserved36[10];
	__be16 pp_size; /* log2(pp_size) */
	char reserved46[12];
	__be16 version;
	};

struct vgda {
	__be32 secs;
	__be32 usec;
	char reserved8[16];
	__be16 numlvs;
	__be16 maxlvs;
	__be16 pp_size;
	__be16 numpvs;
	__be16 total_vgdas;
	__be16 vgda_size;
	};

struct lvd {
	__be16 lv_ix;
	__be16 res2;
	__be16 res4;
	__be16 maxsize;
	__be16 lv_state;
	__be16 mirror;
	__be16 mirror_policy;
	__be16 num_lps;
	__be16 res10[8];
	};

struct lvname {
	char name[64];
	};

struct ppe {
	__be16 lv_ix;
	unsigned short res2;
	unsigned short res4;
	__be16 lp_ix;
	unsigned short res8[12];
	};

struct pvd {
	char reserved0[16];
	__be16 pp_count;
	char reserved18[2];
	__be32 psn_part1;
	char reserved24[8];
	struct ppe ppe[1016];
	};

#define LVM_MAXLVS 256

/**
 * read_lba(): Read bytes from disk, starting at given LBA
 * @state
 * @lba
 * @buffer
 * @count
 *
 * Description:  Reads @count bytes from @state->bdev into @buffer.
 * Returns number of bytes read on success, 0 on error.
 */
static size_t read_lba(int fd, u32 lba, u8 * buffer, size_t count)
{
	size_t totalreadcount = 0;

	while (count) {
		int copied = 512;
		char *data = getblock(fd, lba++);
		if (!data)
			break;
		if (copied > count)
			copied = count;
		memcpy(buffer, data, copied);
		buffer += copied;
		totalreadcount +=copied;
		count -= copied;
	}
	return totalreadcount;
}

/**
 * alloc_pvd(): reads physical volume descriptor
 * @state
 * @lba
 *
 * Description: Returns pvd on success,  NULL on error.
 * Allocates space for pvd and fill it with disk blocks at @lba
 * Notes: remember to free pvd when you're done!
 */
static struct pvd *alloc_pvd(int fd, u32 lba)
{
	size_t count = sizeof(struct pvd);
	struct pvd *p;

	p = malloc(count);
	if (!p)
		return NULL;

	if (read_lba(fd, lba, (u8 *) p, count) < count) {
		free(p);
		return NULL;
	}
	return p;
}

/**
 * alloc_lvn(): reads logical volume names
 * @state
 * @lba
 *
 * Description: Returns lvn on success,  NULL on error.
 * Allocates space for lvn and fill it with disk blocks at @lba
 * Notes: remember to free lvn when you're done!
 */
static struct lvname *alloc_lvn(int fd, u32 lba)
{
	size_t count = sizeof(struct lvname) * LVM_MAXLVS;
	struct lvname *p;

	p = malloc(count);
	if (!p)
		return NULL;

	if (read_lba(fd, lba, (u8 *) p, count) < count) {
		free(p);
		return NULL;
	}
	return p;
}

int
read_aixlvm_pt(int fd, struct slice all, struct slice *sp, int ns) {
	unsigned long offset = all.start;
	char *bp;
	u32 pp_bytes_size;
	u32 pp_blocks_size = 0;
	u32 vgda_sector = 0;
	u32 vgda_len = 0;
	int numlvs = 0;
	struct lv_info {
		unsigned short pps_per_lv;
		unsigned short pps_found;
		unsigned char lv_is_contiguous;
	} *lvip;
	struct lvname *n = NULL;
	struct pvd *pvd;

	bp = getblock(fd, offset);
	if (bp == NULL)
		return -1;

	/* IBMA in EBCDIC */
	if (memcmp(bp, "\xc9\xc2\xd4\xc1", 4) != 0)
		return -1;

	bp = getblock(fd, offset + 7);
	if (bp == NULL)
		return -1;
	else {
		struct lvm_rec *p = (struct lvm_rec *)bp;
		u16 lvm_version = be16_to_cpu(p->version);

		if (memcmp(p->lvm_id, "_LVM", sizeof p->lvm_id) != 0)
			return -1;
		if (lvm_version == 1) {
			int pp_size_log2 = be16_to_cpu(p->pp_size);

			pp_bytes_size = 1 << pp_size_log2;
			pp_blocks_size = pp_bytes_size / 512;
			printf(" AIX LVM header version %u found\n",
				lvm_version);
			vgda_len = be32_to_cpu(p->vgda_len);
			vgda_sector = be32_to_cpu(p->vgda_psn[0]);
		} else {
			printf(" unsupported AIX LVM version %d found\n",
				lvm_version);
			return 0;
		}
	}

	if (vgda_sector && (bp = getblock(fd, offset + vgda_sector))) {
		struct vgda *p = (struct vgda *)bp;

		numlvs = be16_to_cpu(p->numlvs);
		printf("numlvs = %d\n", numlvs);
	}

	lvip = calloc(sizeof(struct lv_info), ns);
	if (!lvip)
		return 0;
	if (numlvs && (bp = getblock(fd, offset + vgda_sector + 1))) {
		struct lvd *p = (struct lvd *)bp;
		int i;

		n = alloc_lvn(fd, vgda_sector + vgda_len - 33);
		if (n) {
			int foundlvs = 0;

			for (i = 0; foundlvs < numlvs && i < ns; i += 1) {
				lvip[i].pps_per_lv = be16_to_cpu(p[i].num_lps);
				if (lvip[i].pps_per_lv)
					foundlvs += 1;
			}
		}
	}

	pvd = alloc_pvd(fd, vgda_sector + 17);
	if (pvd) {
		int numpps = be16_to_cpu(pvd->pp_count);
		int psn_part1 = be32_to_cpu(pvd->psn_part1);
#if 0
		int i;
		int cur_lv_ix = -1;
		int next_lp_ix = 1;
		int lp_ix;

		for (i = 0; i < numpps; i += 1) {
			struct ppe *p = pvd->ppe + i;
			unsigned int lv_ix;

			lp_ix = be16_to_cpu(p->lp_ix);
			if (!lp_ix) {
				next_lp_ix = 1;
				continue;
			}
			lv_ix = be16_to_cpu(p->lv_ix) - 1;
			if (lv_ix >= ns) {
				cur_lv_ix = -1;
				continue;
			}
			lvip[lv_ix].pps_found += 1;
			if (lp_ix == 1) {
				cur_lv_ix = lv_ix;
				next_lp_ix = 1;
			} else if (lv_ix != cur_lv_ix || lp_ix != next_lp_ix) {
				next_lp_ix = 1;
				continue;
			}
			if (lp_ix == lvip[lv_ix].pps_per_lv) {
				sp[lv_ix].start = (i + 1 - lp_ix) * pp_blocks_size + psn_part1;
				sp[lv_ix].size = lvip[lv_ix].pps_per_lv * pp_blocks_size;
				printf(" <%s>\n", n[lv_ix].name);
				lvip[lv_ix].lv_is_contiguous = 1;
				next_lp_ix = 1;
			} else
				next_lp_ix += 1;
		}
		for (i = 0; i < ns; i += 1)
			if (lvip[i].pps_found && !lvip[i].lv_is_contiguous)
				printf("partition %s (%u pp's found) is not contiguous\n",
					n[i].name, lvip[i].pps_found);
#else
		int i;
		unsigned short *pps_by_lvs;
		unsigned short *start_of_pps_array_of_lv;

		pps_by_lvs = calloc(1016, sizeof(unsigned short));
		start_of_pps_array_of_lv = calloc(numlvs, sizeof(unsigned short));
		start_of_pps_array_of_lv[0] = 0;
		for (i = 0; i < numlvs - 1; i += 1)
			start_of_pps_array_of_lv[i + 1] = start_of_pps_array_of_lv[i] + lvip[i].pps_per_lv;
		for (i = 0; i < numpps; i += 1) {
			struct ppe *p = pvd->ppe + i;
			unsigned int lp_ix;
			unsigned int lv_ix;

			lp_ix = be16_to_cpu(p->lp_ix);
			if (!lp_ix)
				continue;
			lv_ix = be16_to_cpu(p->lv_ix) - 1;
			pps_by_lvs[start_of_pps_array_of_lv[lv_ix] + lp_ix - 1] = i;
		}
		for (i = 0; i < numlvs; i += 1) {
			int j;

			printf("%s:", n[i].name);
			for (j = 0; j < lvip[i].pps_per_lv; j += 1)
				printf(" %u", pps_by_lvs[start_of_pps_array_of_lv[i] + j] + 1);
			printf("\n");
		}
		for (i = 0; i < numlvs; i += 1) {
			int j;
			int first_pp, cur_pp;

			printf("%s:", n[i].name);
			for (j = 0; j < lvip[i].pps_per_lv; j += 1) {
				int this_pp = pps_by_lvs[start_of_pps_array_of_lv[i] + j] + 1;

				if (j == 0) {
					first_pp = this_pp;
					cur_pp = this_pp;
				} else if (this_pp == cur_pp + 1)
					cur_pp = this_pp;
				else {
					if (cur_pp != first_pp)
						printf(" %u-%u", first_pp, cur_pp);
					else
						printf(" %u", first_pp);
					first_pp = this_pp;
					cur_pp = this_pp;
				}
			}
			if (cur_pp != first_pp)
				printf(" %u-%u\n", first_pp, cur_pp);
			else
				printf(" %u\n", first_pp);
		}
		for (i = 0; i < numlvs; i += 1) {
			int j, first_j = 0;
			int first_pp, cur_pp;

			printf("dmsetup create %s << EOT\n", n[i].name);
			for (j = 0; j < lvip[i].pps_per_lv; j += 1) {
				int this_pp = pps_by_lvs[start_of_pps_array_of_lv[i] + j] + 1;

				if (j == 0) {
					first_pp = this_pp;
					cur_pp = this_pp;
				} else if (this_pp == cur_pp + 1)
					cur_pp = this_pp;
				else {
					printf("%u %u linear $DEVICE %u\n",
						first_j * pp_blocks_size,
						(cur_pp - first_pp + 1) * pp_blocks_size,
						(first_pp - 1) * pp_blocks_size + psn_part1);
					first_j = j;
					first_pp = this_pp;
					cur_pp = this_pp;
				}
			}
			if (j)
				printf("%u %u linear $DEVICE %u\n",
					first_j * pp_blocks_size,
					(cur_pp - first_pp + 1) * pp_blocks_size,
					(first_pp - 1) * pp_blocks_size + psn_part1);
			printf("EOT\n");
		}
		free(start_of_pps_array_of_lv);
		free(pps_by_lvs);
#endif
		free(pvd);
	}
	if (n)
		free(n);
	free(lvip);
	return numlvs;
}

/*
 * dirent.c
 *
 * Include functions implementing directory entry.
 * implementation file.
 *
 * knightray@gmail.com
 * 10/30 2008
 */

#include "dirent.h"
#include "debug.h"
#include "fat.h"
#include "common.h"
#include "dir.h"

#ifndef DBG_DIRENT
#undef DBG
#define DBG nulldbg
#endif

/* Private routins declaration. */

static int32
_get_dirent(
	IN	tdir_t * pdir,
	OUT	dir_entry_t * pdirent
);

static void
_parse_file_name(
	IN	tdir_t * pdir,
	IN	dir_entry_t * pdirent,
	OUT	tdir_entry_t * pdir_entry
);

static BOOL
_get_long_file_name(
    IN  dir_entry_t * pdirent,
    OUT byte * long_name
);

static BOOL
_convert_short_fname(
	IN	ubyte * dir_name,
	OUT	byte * d_name
);

static BOOL
_convert_to_short_fname(
	IN	byte * fname,
	OUT	ubyte * short_fname
);

static ubyte
_get_check_sum(
	IN	ubyte * fname
);

/*----------------------------------------------------------------------------------------------------*/

int32
dirent_find(
	IN	tdir_t * pdir,
	IN	byte * dirname,
	OUT	tdir_entry_t * pdir_entry)
{
	DBG("%s start\n", __FUNCTION__);
	int32 ret;

	pdir->cur_clus = pdir->start_clus;
	pdir->cur_sec = 0;
	pdir->cur_dir_entry = 0;
	if (dir_read_sector(pdir) != DIR_OK)
		return ERR_DIRENTRY_NOT_FOUND;

#ifdef _KERNEL_
	ASSERT(pdir_entry->pdirent == NULL);
#endif

	ret = DIRENTRY_OK;

	while(1) {
		dir_entry_t dirent;
		if ((ret = _get_dirent(pdir, &dirent)) == DIRENTRY_OK)	{
			if (dirent.dir_name[0] == 0x00) {
				ret = ERR_DIRENTRY_NOT_FOUND;
				break;
			} else if (dirent.dir_name[0] == 0xE5) {
				//FixMe do with 0x5
				continue;
			}

			memset(pdir_entry->long_name, 0, LONG_NAME_LEN);
			memset(pdir_entry->short_name, 0, SHORT_NAME_LEN);
			_parse_file_name(pdir, &dirent, pdir_entry);

			if (!strcmp(pdir_entry->long_name, dirname)) {
				break;
			}
		} else if (ret == ERR_DIRENTRY_NOMORE_ENTRY){
			ret = ERR_DIRENTRY_NOT_FOUND;
			break;
		} else {
			break;
		}
	}

	return ret;
}

BOOL
dirent_is_empty(
	IN	tdir_t * pdir)
{
	BOOL ret;
	tdir_entry_t * pdir_entry;

	pdir->cur_clus = pdir->start_clus;
	pdir->cur_sec = 0;
	pdir->cur_dir_entry = 0;
	if (dir_read_sector(pdir) != DIR_OK)
		return ERR_DIRENTRY_NOT_FOUND;

	pdir_entry = dirent_malloc();

#ifdef _KERNEL_
	ASSERT(pdir_entry->pdirent == NULL);
#endif

	ret = TRUE;

	while(1) {
		dir_entry_t dirent;

		if ((ret = _get_dirent(pdir, &dirent)) == DIRENTRY_OK)	{

			if (dirent.dir_name[0] == 0x00) {
				break;
			}
			else if (dirent.dir_name[0] == 0xE5) {
				//FixMe do with 0x5
				continue;
			}

			memset(pdir_entry->long_name, 0, LONG_NAME_LEN);
			memset(pdir_entry->short_name, 0, SHORT_NAME_LEN);
			_parse_file_name(pdir, &dirent, pdir_entry);

			if (!strcmp(pdir_entry->long_name, ".") || !strcmp(pdir_entry->long_name, "..")) {
				continue;
			}
			else {
				ret = FALSE;
				break;
			}
		}
		else if (ret == ERR_DIRENTRY_NOMORE_ENTRY){
			break;
		}
		else {
			DBG("%s(): get next direntry failed. ret = %d\n", __FUNCTION__, ret);
			ret = FALSE;
			break;
		}
	}

	dirent_release(pdir_entry);
	return ret;
}

int32
dirent_find_free_entry(
	IN	tdir_t * pdir,
	IN	tdir_entry_t * pdir_entry)
{
	DBG("%s\n", __FUNCTION__);
	int32 ret;
	tffs_t * ptffs = pdir->ptffs;
	int32 seq_num = 0;

	ret = DIRENTRY_OK;

	while(1) {
		dir_entry_t dirent;

		if ((ret = _get_dirent(pdir, &dirent)) == DIRENTRY_OK)	{
			DBG("%s() : DIRENTRY_OK\n", __FUNCTION__);
			if (dirent.dir_name[0] == 0x00) {
				pdir->cur_dir_entry--;
				break;
			} else if (dirent.dir_name[0] == 0xE5) {
				seq_num++;
				if (seq_num >= pdir_entry->dirent_num) {
					pdir->cur_dir_entry -= seq_num;
					break;
				} else {
					continue;
				}
			}

			seq_num = 0;
		} else if (ret == ERR_DIRENTRY_NOMORE_ENTRY) {
			DBG("%s() : ERR_DIRENTRY_NOMORE_ENTRY\n", __FUNCTION__);
			uint32 new_clus;

			dir_write_sector(pdir);
			if ((ret = fat_malloc_clus(ptffs->pfat, pdir->cur_clus, &new_clus)) == FAT_OK) {
				pdir->cur_clus = new_clus;
				pdir->cur_sec = 0;
				pdir->cur_dir_entry = 0;
				dir_read_sector(pdir);
			} else {
				ret = ERR_DIRENTRY_NOMORE_ENTRY;
				break;
			}
		} else {
			DBG("%s() : ELSE\n", __FUNCTION__);
			//break;
		}
	}

	return ret;
}

int32
dirent_get_next(
	IN	tdir_t * pdir,
	OUT	tdir_entry_t * pdir_entry)
{
	int32 ret;

	ret = DIRENTRY_OK;

	while(1)	{
		dir_entry_t dirent;

		//print_sector(pdir->secbuf, 1);
		DBG("%s():pdir->cur_clus = %d, pdir->cur_dir_entry = %d\n", __FUNCTION__, pdir->cur_clus, pdir->cur_dir_entry);
		if ((ret = _get_dirent(pdir, &dirent)) == DIRENTRY_OK)	{

			if (dirent.dir_name[0] == 0x00) {
				ret = ERR_DIRENTRY_NOMORE_ENTRY;
				break;
			}
			else if (dirent.dir_name[0] == 0xE5) {
				//FixMe do with 0x5
				continue;
			}

			memset(pdir_entry->long_name, 0, LONG_NAME_LEN);
			memset(pdir_entry->short_name, 0, SHORT_NAME_LEN);
			_parse_file_name(pdir, &dirent, pdir_entry); 

			break;
		}
		else {
			break;
		}
	}
	return ret;
}

void
dirent_release(
	IN	tdir_entry_t * pdir_entry)
{
	free(pdir_entry->pdirent);
	free(pdir_entry);
}

tdir_entry_t *
dirent_malloc()
{
	tdir_entry_t * pdir_entry;

	pdir_entry = (tdir_entry_t *)malloc(sizeof(tdir_entry_t));
	memset(pdir_entry, 0, sizeof(tdir_entry_t));
	return pdir_entry;
}

BOOL
dirent_init(
	IN	byte * fname,
	IN	ubyte dir_attr,
	IN	byte use_long_name,
	OUT	tdir_entry_t * pdir_entry)
{
	DBG("%s\n", __FUNCTION__);
	long_dir_entry_t * plfent;
	dir_entry_t * pdirent;
	uint32 lfent_num;
	int32 lfent_i;
	byte * pfname;

	if (strlen(fname) > LONG_NAME_LEN || (!use_long_name && strlen(fname) > SHORT_NAME_LEN))
		return FALSE;

	if (use_long_name) {
		lfent_num = strlen(fname) / 13 + 2;
	} else {
		lfent_num = 1;
	}

	pfname = fname;

	plfent = (long_dir_entry_t *)malloc(sizeof(long_dir_entry_t) * lfent_num);
	memset(plfent, 0, sizeof(long_dir_entry_t) * lfent_num);
	pdirent = (dir_entry_t *)(&plfent[lfent_num - 1]);
	
	_convert_to_short_fname(fname, pdirent->dir_name);
	DBG("%s(): %s=>%s\n", __FUNCTION__, fname, pdirent->dir_name);
	pdirent->dir_attr = dir_attr;
	pdirent->dir_ntres = 0;
	pdirent->dir_crt_time_tenth = dirent_get_cur_time_tenth();
	pdirent->dir_crt_time = dirent_get_cur_time();
	pdirent->dir_crt_date = dirent_get_cur_date();
	pdirent->dir_lst_acc_date = pdirent->dir_crt_date;
	pdirent->dir_wrt_time = pdirent->dir_crt_time;
	pdirent->dir_wrt_date = pdirent->dir_crt_date;
	pdirent->dir_fst_clus_hi = 0;
	pdirent->dir_fst_clus_lo = 0;
	pdirent->dir_file_size = 0;

	if (use_long_name) {
		for (lfent_i = lfent_num - 2; lfent_i >= 0; lfent_i--) {
			ubyte fname_line[13];

			memset(fname_line, 0xFF, 13);
			memcpy(fname_line, pfname, min(fname + strlen(fname) - pfname + 1, 13));

			if (lfent_i == 0) {
				plfent[lfent_i].ldir_ord = (lfent_num - 1 - lfent_i) | LAST_LONG_ENTRY;
			} else {
				plfent[lfent_i].ldir_ord = lfent_num - 1 - lfent_i;
			}

			copy_to_unicode(fname_line, 5, plfent[lfent_i].ldir_name1);
			copy_to_unicode(fname_line + 5, 6, plfent[lfent_i].ldir_name2);
			copy_to_unicode(fname_line + 11, 2, plfent[lfent_i].ldir_name3);
			pfname += 13;

			plfent[lfent_i].ldir_attr = ATTR_LONG_NAME;
			plfent[lfent_i].ldir_type = 0;
			plfent[lfent_i].ldir_chksum = _get_check_sum(pdirent->dir_name);
			plfent[lfent_i].ldir_fst_clus_lo = 0;
		}
	}

	pdir_entry->pdirent = (dir_entry_t *)plfent;
	pdir_entry->dirent_num = lfent_num;
	strcpy(pdir_entry->long_name, fname);	
	_convert_short_fname(pdirent->dir_name, pdir_entry->short_name);

	return TRUE;
}

uint16
dirent_get_cur_time()
{
	tffs_sys_time_t curtm;
	uint16 ret;

	Getcurtime(&curtm);	

	ret = 0;
	ret |= (curtm.tm_sec >> 2) & 0x1F;
	ret |= (curtm.tm_min << 5) & 0x7E0;
	ret |= (curtm.tm_hour << 11) & 0xF800;
	
	return ret;
}

uint16
dirent_get_cur_date()
{
	tffs_sys_time_t curtm;
	uint16 ret;

	Getcurtime(&curtm);	
	
	ret = 0;
	ret |= (curtm.tm_mday) & 0x1F;
	ret |= ((curtm.tm_mon + 1) << 5) & 0x1E0;
	ret |= ((curtm.tm_year - 80) << 9) & 0xFE00;
	
	return ret;
}

ubyte
dirent_get_cur_time_tenth()
{
	tffs_sys_time_t curtm;
	uint16 ret;

	Getcurtime(&curtm);	
	
	ret = (curtm.tm_sec & 1) == 0 ? 0 : 100;
	return ret;
}

/*----------------------------------------------------------------------------------------------------*/

static int32
_get_dirent(
	IN	tdir_t * pdir,
	OUT	dir_entry_t * pdirent)
{
	int32 ret;
	boot_sector_t* pbs = pdir->ptffs->pbs;

	ret = DIRENTRY_OK;
	
	if (pdir->cur_dir_entry < (pbs->byts_per_sec * pbs->sec_per_clus / sizeof(dir_entry_t))) {
		memcpy(pdirent, (dir_entry_t *)pdir->secbuf + pdir->cur_dir_entry, sizeof(dir_entry_t));
		pdir->cur_dir_entry++;
	} else {
		if (fat_get_next_sec(pdir->ptffs->pfat, &pdir->cur_clus, &pdir->cur_sec)) {
			pdir->cur_dir_entry = 0;
			if ((ret = dir_read_sector(pdir)) == DIR_OK) {
				memcpy(pdirent, (dir_entry_t *)pdir->secbuf + pdir->cur_dir_entry, sizeof(dir_entry_t));
				pdir->cur_dir_entry++;
			} else {
				ret = ERR_DIRENTRY_DEVICE_FAIL;
			}
		} else {
			ret = ERR_DIRENTRY_NOMORE_ENTRY;
		}
	}

	return ret;
}

static void
_parse_file_name(
	IN	tdir_t * pdir,
	IN	dir_entry_t * pdirent,
	OUT	tdir_entry_t * pdir_entry)
{
	int32 lf_entry_num;

	lf_entry_num = 0;
	if (pdirent->dir_attr & ATTR_LONG_NAME) {
		uint32 lf_i;
		dir_entry_t dirent;

		lf_entry_num = pdirent->dir_name[0] & ~(LAST_LONG_ENTRY);
		pdir_entry->pdirent = (dir_entry_t *)malloc((lf_entry_num + 1) * sizeof(dir_entry_t));

		_get_long_file_name(pdirent, pdir_entry->long_name + (lf_entry_num - 1) * 13);
		memcpy(pdir_entry->pdirent, pdirent, sizeof(dir_entry_t));

		for (lf_i = 1; lf_i < lf_entry_num; lf_i++) {
			_get_dirent(pdir, &dirent);
			memcpy(pdir_entry->pdirent + lf_i, &dirent, sizeof(dir_entry_t));
			_get_long_file_name(&dirent, pdir_entry->long_name + (lf_entry_num - lf_i - 1) * 13);
		}

		_get_dirent(pdir, &dirent);
		memcpy(pdir_entry->pdirent + lf_i, &dirent, sizeof(dir_entry_t));
	}
	else {
		pdir_entry->pdirent = (dir_entry_t *)malloc(sizeof(dir_entry_t));

		_convert_short_fname(pdirent->dir_name, pdir_entry->long_name);
		memcpy(pdir_entry->pdirent, pdirent, sizeof(dir_entry_t));
	}

	_convert_short_fname(pdirent->dir_name, pdir_entry->short_name);
	pdir_entry->dirent_num = lf_entry_num + 1;
}

static ubyte
_get_check_sum(
	IN	ubyte * fname)
{
	int16 fname_len;
	ubyte sum;

	sum = 0;
	for (fname_len = 11; fname_len != 0; fname_len--) {
		sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *fname++;
	}
	return sum;
}

int _toup(int c) {
	return (c >= 'a' && c <= 'z') ? (c - 32) : c;
}

static BOOL
_convert_to_short_fname(
	IN	byte * fname,
	OUT	ubyte * short_fname)
{
	byte * pfname;
	byte * pcur;
	uint16 sf_i;
	static uint16 num_tail = 0;

	for (sf_i = 0; sf_i < 11; sf_i++)
		short_fname[sf_i] = ' ';

	/* FixMe! . and .. can not handle probably  */
	if (!strcmp(fname, ".") || !strcmp(fname, "..")) {
		strcpy((byte *)short_fname, fname);
		return TRUE;
	}		

	pfname = dup_string(fname);
	pcur = pfname;
	sf_i = 0;

	trip_blanks(pfname);

	while (sf_i < 8) {
		if (*pcur == '\0' || *pcur == '.') {
			break;
		}
		short_fname[sf_i++] = _toup(*pcur++);
	}

	if (*pcur == '.') {
		pcur++;
	}
	else {
		if (*pcur != '\0') {
			byte str_tail[8];

			while(*pcur && *pcur != '.')
				pcur++;

			if (*pcur == '.')
				pcur++;

			sprintf(str_tail, "~%d", num_tail++);\
			memcpy(short_fname + (8 - strlen(str_tail)), str_tail, strlen(str_tail));

			if (*pcur == '\0')
				goto _release;
		}
	}

	sf_i = 8;

	while (sf_i < 11) {
		if (*pcur == '\0')
			break;
		short_fname[sf_i++] = _toup(*pcur++);
	}

_release:
	free(pfname);
	return TRUE;
}

static BOOL
_get_long_file_name(
	IN	dir_entry_t * pdirent,
	OUT	byte * long_name)
{
	long_dir_entry_t * pldirent;

	pldirent = (long_dir_entry_t *)pdirent;

	copy_from_unicode(pldirent->ldir_name1, 5, long_name);	
	copy_from_unicode(pldirent->ldir_name2, 6, long_name + 5);	
	copy_from_unicode(pldirent->ldir_name3, 2, long_name + 11);	
	return TRUE;
}

static BOOL
_convert_short_fname(
	IN	ubyte * dir_name,
	OUT	byte * d_name)
{
	uint32 i;

	memset(d_name, 0, 11);
	for (i = 0; i < 8; i++) {
		if (dir_name[i] == ' ')
			break;
		d_name[i] = dir_name[i];
	}

	if (dir_name[8] != ' ') {
		uint32 j;

		d_name[i++] = '.';
		for (j = 0; j < 3; j++) {
			if (dir_name[8 + j] == ' ')
				break;
			d_name[i + j] = dir_name[8 + j];
		}
	}
	return TRUE;
}


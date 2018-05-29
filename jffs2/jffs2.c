/*
 * Phoenix-RTOS
 *
 * Phoenix file server
 *
 * jffs2
 *
 * Copyright 2012, 2016, 2018 Phoenix Systems
 * Copyright 2007 Pawel Pisarczyk
 * Author: Kamil Amanowicz
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/msg.h>
#include <sys/list.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include "os-phoenix.h"
#include "os-phoenix/object.h"
#include "nodelist.h"

#define JFFS2_ROOT_DIR &jffs2_common.root

extern int jffs2_readdir(struct file *file, struct dir_context *ctx);

static int jffs2_srv_lookup(oid_t *dir, const char *name, oid_t *res)
{
	struct dentry *dentry, *dtemp;
	struct inode *inode = NULL;
	int len = 0;
	char *end;

	if (dir->id == 0)
		dir->id = 1;

	res->id = 0;

	inode = jffs2_iget(jffs2_common.sb, dir->id);

	if (IS_ERR(inode))
		return -EINVAL;

	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}

	dentry = malloc(sizeof(struct dentry));
	res->port = jffs2_common.port;

	while (name[len] != '\0') {
		while (name[len] == '/') {
			len++;
			continue;
		}

		dentry->d_name.name = strdup(name + len);

		end = strchr(dentry->d_name.name, '/');
		if (end != NULL)
			*end = 0;

		if (!strcmp(dentry->d_name.name, ".")) {
			res->id = inode->i_ino;
			len++;
			free(dentry->d_name.name);
			dentry->d_name.len = 0;
			continue;
		} else if (!strcmp(dentry->d_name.name, "..")) {
			res->id = JFFS2_INODE_INFO(inode)->inocache->pino_nlink;
			len += 2;
			free(dentry->d_name.name);
			dentry->d_name.len = 0;
			inode = jffs2_iget(jffs2_common.sb, res->id);
			continue;
		}

		dentry->d_name.len = strlen(dentry->d_name.name);

		dtemp = inode->i_op->lookup(inode, dentry, 0);

		if (dtemp == NULL) {
			free(dentry->d_name.name);
			break;
		} else
			res->id = dtemp->d_inode->i_ino;

		len += dentry->d_name.len;
		free(dentry->d_name.name);
		dentry->d_name.len = 0;

		iput(inode);
		inode = d_inode(dtemp);
	}

	free(dentry);
	iput(inode);

	if (!res->id)
		return -ENOENT;

	return len;
}


static int jffs2_srv_setattr(oid_t *oid, int type, int attr)
{
	struct iattr iattr;
	struct inode *inode;
	struct jffs2_inode_info *f;
	struct dentry dentry;
	int ret;

	inode = jffs2_iget(jffs2_common.sb, oid->id);
	if (IS_ERR(inode))
		return -ENOENT;

	f = JFFS2_INODE_INFO(inode);

	mutex_lock(&f->sem);

	switch (type) {

		case (atMode): /* mode */
			iattr.ia_valid = ATTR_MODE;
			iattr.ia_mode = (inode->i_mode & ~0xffff) | (attr & 0xffff);
			break;

		case (atUid): /* uid */
			iattr.ia_valid = ATTR_UID;
			iattr.ia_uid.val = attr;
			break;

		case (atGid): /* gid */
			iattr.ia_valid = ATTR_GID;
			iattr.ia_gid.val = attr;
			break;

		case (atSize): /* size */
			iattr.ia_valid = ATTR_SIZE;
			iattr.ia_size = attr;
			break;

		case (atPort): /* port */
			inode->i_rdev = attr;
			break;
	}

	d_instantiate(&dentry, inode);

	mutex_unlock(&f->sem);

	ret = inode->i_op->setattr(&dentry, &iattr);
	iput(inode);

	return ret;
}


static int jffs2_srv_getattr(oid_t *oid, int type, int *attr)
{
	struct inode *inode;
	struct jffs2_inode_info *f;

	if (!oid->id)
		return -EINVAL;

	inode = jffs2_iget(jffs2_common.sb, oid->id);

	if (IS_ERR(inode))
		return -ENOENT;

	f = JFFS2_INODE_INFO(inode);

	mutex_lock(&f->sem);
	switch (type) {

		case (atMode): /* mode */
			*attr = inode->i_mode;
			break;

		case (atUid): /* uid */
			*attr = inode->i_uid.val;
			break;

		case (atGid): /* gid */
			*attr = inode->i_gid.val;
			break;

		case (atSize): /* size */
			*attr = inode->i_size;
			break;

		case (atType): /* type */
			if (S_ISDIR(inode->i_mode))
				*attr = otDir;
			else if (S_ISREG(inode->i_mode))
				*attr = otFile;
			else if (S_ISCHR(inode->i_mode))
				*attr = otDev;
			else
				*attr = otUnknown;
			break;

		case (atPort): /* port */
			*attr = inode->i_rdev;
			break;
	}

	mutex_unlock(&f->sem);
	iput(inode);

	return EOK;
}


static int jffs2_srv_link(oid_t *dir, const char *name, oid_t *oid)
{
	struct inode *idir, *inode;
	struct dentry *old, *new;
	int ret;

	if (!dir->id || !oid->id)
		return -EINVAL;

	if (name == NULL || !strlen(name))
		return -EINVAL;

	idir = jffs2_iget(jffs2_common.sb, dir->id);

	if (IS_ERR(idir))
		return -ENOENT;

	inode = jffs2_iget(jffs2_common.sb, oid->id);

	if (IS_ERR(inode)) {
		iput(idir);
		return -ENOENT;
	}

	old = malloc(sizeof(struct dentry));
	new = malloc(sizeof(struct dentry));

	new->d_name.name = strdup(name);
	new->d_name.len = strlen(name);

	d_instantiate(old, inode);

	ret = idir->i_op->link(old, idir, new);

	iput(idir);
	iput(inode);

	free(old);
	free(new->d_name.name);
	free(new);

	return ret;
}


static int jffs2_srv_unlink(oid_t *dir, const char *name)
{
	struct inode *idir, *inode;
	struct dentry *dentry;
	oid_t oid;
	int ret;

	if (!dir->id)
		return -EINVAL;

	if (name == NULL || !strlen(name))
		return -EINVAL;

	idir = jffs2_iget(jffs2_common.sb, dir->id);

	if (IS_ERR(idir))
		return -ENOENT;

	if (jffs2_srv_lookup(dir, name, &oid) != EOK) {
		iput(idir);
		return -ENOENT;
	}

	inode = jffs2_iget(jffs2_common.sb, oid.id);

	if (IS_ERR(inode)) {
		iput(idir);
		return -ENOENT;
	}

	dentry = malloc(sizeof(struct dentry));

	dentry->d_name.name = strdup(name);
	dentry->d_name.len = strlen(name);

	d_instantiate(dentry, inode);

	ret = idir->i_op->unlink(idir, dentry);

	iput(idir);
	iput(inode);

	free(dentry->d_name.name);
	free(dentry);

	return ret;
}


static int jffs2_srv_create(oid_t *dir, const char *name, oid_t *oid, int type, int mode, u32 port)
{
	oid_t toid = { 0 };
	struct inode *idir;
	struct dentry *dentry;
	int ret = 0;

	idir = jffs2_iget(jffs2_common.sb, dir->id);

	if (IS_ERR(idir))
		return -ENOENT;

	if (!S_ISDIR(idir->i_mode)) {
		iput(idir);
		return -ENOTDIR;
	}

	if ((ret = jffs2_srv_lookup(dir, name, &toid)) == EOK) {
		iput(idir);
		return -EEXIST;
	}

	dentry = malloc(sizeof(struct dentry));
	dentry->d_name.name = strdup(name);
	dentry->d_name.len = strlen(name);

	switch (type) {
		case otFile:
			mode = S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO;
			oid->port = jffs2_common.port;
			ret = idir->i_op->create(idir, dentry, mode, 0);
			break;
		case otDir:
			oid->port = jffs2_common.port;
			ret = idir->i_op->mkdir(idir, dentry, mode);
		default:
			ret = -EINVAL;
			break;
	}

	iput(idir);

	if (!ret)
		oid->id = d_inode(dentry)->i_ino;

	return ret;
}


static int jffs2_srv_destroy(oid_t *oid)
{
	return 0;
}


static int jffs2_srv_readdir(oid_t *dir, offs_t offs, struct dirent *dent, unsigned int size)
{
	struct inode *inode;
	struct file file;
	struct dir_context ctx = {dir_print, offs, dent, -1};

	if (!dir->id)
		return -EINVAL;

	inode = jffs2_iget(jffs2_common.sb, dir->id);

	if (IS_ERR(inode))
		return -EINVAL;

	if (!((inode->i_mode >> 14) & 1))
		return -EINVAL;

	file.f_pino = JFFS2_INODE_INFO(inode)->inocache->pino_nlink;
	file.f_inode = inode;

	jffs2_readdir(&file, &ctx);
	iput(inode);

	return ctx.emit;
}


static void jffs2_srv_open(oid_t *oid)
{
}


static void jffs2_srv_close(oid_t *oid)
{
}


static int jffs2_srv_read(oid_t *oid, offs_t offs, void *data, unsigned long len)
{
	struct inode *inode;
	struct jffs2_inode_info *f;
	struct jffs2_sb_info *c;
	int ret;

	if (!oid->id)
		return -EINVAL;

	inode = jffs2_iget(jffs2_common.sb, oid->id);

	f = JFFS2_INODE_INFO(inode);
	c = JFFS2_SB_INFO(inode->i_sb);

	if (IS_ERR(inode))
		return -EINVAL;

	if(S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -EISDIR;
	}

	f = JFFS2_INODE_INFO(inode);
	c = JFFS2_SB_INFO(inode->i_sb);

	if (inode->i_size < offs)
		return 0;

	mutex_lock(&f->sem);
	ret = jffs2_read_inode_range(c, f, data, offs, len);
	mutex_unlock(&f->sem);

	if (!ret)
		ret = len > inode->i_size - offs ? inode->i_size - offs : len;
	else
		printf("jffs2: read error %d\n", ret);

	iput(inode);
	return ret;
}



static int jffs2_srv_prepare_write(struct inode *inode, loff_t offs, unsigned long len)
{
	struct jffs2_inode_info *f = JFFS2_INODE_INFO(inode);
	struct jffs2_sb_info *c = JFFS2_SB_INFO(inode->i_sb);
	struct jffs2_raw_inode ri;
	struct jffs2_full_dnode *fn;
	uint32_t alloc_len;
	int ret;

	if (len > inode->i_size) {

		jffs2_dbg(1, "Writing new hole frag 0x%x-0x%x between current EOF and new page\n",
			  (unsigned int)inode->i_size, len);

		ret = jffs2_reserve_space(c, sizeof(ri), &alloc_len,
					  ALLOC_NORMAL, JFFS2_SUMMARY_INODE_SIZE);
		if (ret)
			return ret;

		mutex_lock(&f->sem);
		memset(&ri, 0, sizeof(ri));

		ri.magic = cpu_to_je16(JFFS2_MAGIC_BITMASK);
		ri.nodetype = cpu_to_je16(JFFS2_NODETYPE_INODE);
		ri.totlen = cpu_to_je32(sizeof(ri));
		ri.hdr_crc = cpu_to_je32(crc32(0, &ri, sizeof(struct jffs2_unknown_node) - 4));

		ri.ino = cpu_to_je32(f->inocache->ino);
		ri.version = cpu_to_je32(++f->highest_version);
		ri.mode = cpu_to_jemode(inode->i_mode);
		ri.uid = cpu_to_je16(i_uid_read(inode));
		ri.gid = cpu_to_je16(i_gid_read(inode));
		ri.isize = cpu_to_je32(max((uint32_t)inode->i_size, len));
		ri.atime = ri.ctime = ri.mtime = cpu_to_je32(get_seconds());
		ri.offset = cpu_to_je32(inode->i_size);
		ri.dsize = cpu_to_je32(len - inode->i_size);
		ri.csize = cpu_to_je32(0);
		ri.compr = JFFS2_COMPR_ZERO;
		ri.node_crc = cpu_to_je32(crc32(0, &ri, sizeof(ri)-8));
		ri.data_crc = cpu_to_je32(0);

		fn = jffs2_write_dnode(c, f, &ri, NULL, 0, ALLOC_NORMAL);

		if (IS_ERR(fn)) {
			ret = PTR_ERR(fn);
			jffs2_complete_reservation(c);
			mutex_unlock(&f->sem);
			return ret;
		}

		ret = jffs2_add_full_dnode_to_inode(c, f, fn);

		if (f->metadata) {
			jffs2_mark_node_obsolete(c, f->metadata->raw);
			jffs2_free_full_dnode(f->metadata);
			f->metadata = NULL;
		}

		if (ret) {
			jffs2_dbg(1, "Eep. add_full_dnode_to_inode() failed in write_begin, returned %d\n",
				  ret);
			jffs2_mark_node_obsolete(c, fn->raw);
			jffs2_free_full_dnode(fn);
			jffs2_complete_reservation(c);
			mutex_unlock(&f->sem);

			return ret;
		}
		jffs2_complete_reservation(c);
		inode->i_size = len;
		mutex_unlock(&f->sem);
	}

	return 0;
}

static int jffs2_srv_write(oid_t *oid, offs_t offs, void *data, unsigned long len)
{
	struct inode *inode;
	struct jffs2_inode_info *f;
	struct jffs2_sb_info *c;
	struct jffs2_raw_inode *ri;
	uint32_t writelen = 0;
	int ret;

	if (!oid->id)
		return -EINVAL;

	ri = jffs2_alloc_raw_inode();

	if (ri == NULL) {
		return -ENOMEM;
	}

	inode = jffs2_iget(jffs2_common.sb, oid->id);

	f = JFFS2_INODE_INFO(inode);
	c = JFFS2_SB_INFO(inode->i_sb);

	if (IS_ERR(inode))
		return -EINVAL;

	if(S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -EISDIR;
	}

	if ((ret = jffs2_srv_prepare_write(inode, offs, len))) {
		jffs2_free_raw_inode(ri);
		iput(inode);
		return ret;
	}

	f = JFFS2_INODE_INFO(inode);
	c = JFFS2_SB_INFO(inode->i_sb);

	ri->ino = cpu_to_je32(inode->i_ino);
	ri->mode = cpu_to_jemode(inode->i_mode);
	ri->uid = cpu_to_je16(i_uid_read(inode));
	ri->gid = cpu_to_je16(i_gid_read(inode));
	ri->isize = cpu_to_je32((uint32_t)inode->i_size);
	ri->atime = ri->ctime = ri->mtime = cpu_to_je32(get_seconds());

	ret = jffs2_write_inode_range(c, f, ri, data, offs, len, &writelen);

	if (!ret) {
		if (writelen > inode->i_size) {
			inode->i_size = writelen;
			inode->i_blocks = (inode->i_size + 511) >> 9;
			inode->i_ctime = inode->i_mtime = ITIME(je32_to_cpu(ri->ctime));
		}
	} else
		printf("jffs2: read error %d\n", ret);

	jffs2_free_raw_inode(ri);
	iput(inode);
	return ret;
}


static int jffs2_srv_truncate(oid_t *oid, unsigned long len)
{
	return jffs2_srv_setattr(oid, 3, len);
}


int main(void)
{
	oid_t toid = { 0 };
	msg_t msg;
	unsigned int rid;

	portCreate(&jffs2_common.port);

	printf("jffs2: Starting jffs2 server at port %d\n", jffs2_common.port);

	object_init();
	if(init_jffs2_fs() != EOK) {
		printf("jffs2: Error initialising jffs2\n");
		return -1;
	}

	toid.id = 1;
	/* Try to mount fs as root */
	if (portRegister(jffs2_common.port, "/", &toid) < 0) {
		printf("jffs2: Can't mount on directory %s\n", "/");
		return -1;
	}

	for (;;) {
		if (msgRecv(jffs2_common.port, &msg, &rid) < 0) {
			msgRespond(jffs2_common.port, &msg, rid);
			continue;
		}

		switch (msg.type) {

			case mtOpen:
				jffs2_srv_open(&msg.i.openclose.oid);
				break;

			case mtClose:
				jffs2_srv_close(&msg.i.openclose.oid);
				break;

			case mtRead:
				msg.o.io.err = jffs2_srv_read(&msg.i.io.oid, msg.i.io.offs, msg.o.data, msg.o.size);
				break;

			case mtWrite:
				msg.o.io.err = jffs2_srv_write(&msg.i.io.oid, msg.i.io.offs, msg.i.data, msg.i.size);
				break;

			case mtTruncate:
				msg.o.io.err = jffs2_srv_truncate(&msg.i.io.oid, msg.i.io.len);
				break;

			case mtDevCtl:
				msg.o.io.err = -EINVAL;
				break;

			case mtCreate:
				msg.o.create.err = jffs2_srv_create(&msg.i.create.dir, msg.i.data, &msg.o.create.oid, msg.i.create.type, msg.i.create.mode, msg.i.create.port);
				break;

			case mtDestroy:
				msg.o.io.err = jffs2_srv_destroy(&msg.i.destroy.oid);
				break;

			case mtSetAttr:
				jffs2_srv_setattr(&msg.i.attr.oid, msg.i.attr.type, msg.i.attr.val);
				break;

			case mtGetAttr:
				jffs2_srv_getattr(&msg.i.attr.oid, msg.i.attr.type, &msg.o.attr.val);
				break;

			case mtLookup:
				msg.o.lookup.err = jffs2_srv_lookup(&msg.i.lookup.dir, msg.i.data, &msg.o.lookup.res);
				break;

			case mtLink:
				msg.o.io.err = jffs2_srv_link(&msg.i.ln.dir, msg.i.data, &msg.i.ln.oid);
				break;

			case mtUnlink:
				msg.o.io.err = jffs2_srv_unlink(&msg.i.ln.dir, msg.i.data);
				break;

			case mtReaddir:
				msg.o.io.err = jffs2_srv_readdir(&msg.i.readdir.dir, msg.i.readdir.offs,
						msg.o.data, msg.o.size);
				break;
		}

		msgRespond(jffs2_common.port, &msg, rid);
	}

	return EOK;
}

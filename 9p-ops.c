/*
 * The in-kernel 9p server
 */

#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/statfs.h>
#include <linux/in.h>
#include <linux/namei.h>
#include <linux/net.h>
#include <linux/ipv6.h>
#include <linux/kthread.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/un.h>
#include <linux/uaccess.h>
#include <linux/inet.h>
#include <linux/idr.h>
#include <linux/file.h>
#include <linux/parser.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <net/9p/9p.h>

#include "vhost-9p.h"
#include "protocol.h"

const size_t P9_PDU_HDR_LEN = sizeof(u32) + sizeof(u8) + sizeof(u16);
/*
	TODO:
		- Do not use syscalls
		- Full support of setattr
		- chmod
		- Should have a lock to protect fids
*/

struct p9_server_fid {
	u32 fid;
	u32 uid;
	struct path path;
	struct file *filp;
	struct rb_node node;
};

/* 9p helper routines */

static struct p9_server_fid *lookup_fid(struct p9_server *s, u32 fid_val)
{
	struct rb_node *node = s->fids.rb_node;
	struct p9_server_fid *cur;

	while (node) {
		cur = rb_entry(node, struct p9_server_fid, node);

		if (fid_val < cur->fid)
			node = node->rb_left;
		else if (fid_val > cur->fid)
			node = node->rb_right;
		else
			return cur;
	}

	return ERR_PTR(-ENOENT);
}

static struct p9_server_fid *new_fid(struct p9_server *s, u32 fid_val,
									struct path *path)
{
	struct p9_server_fid *fid;
	struct rb_node **node = &(s->fids.rb_node), *parent = NULL;

    p9s_debug("create fid : %d\n", fid_val);
	while (*node) {
		int result = fid_val - rb_entry(*node, struct p9_server_fid, node)->fid;

		parent = *node;
		if (result < 0)
			node = &((*node)->rb_left);
		else if (result > 0)
			node = &((*node)->rb_right);
		else
			return ERR_PTR(-EEXIST);
	}

	fid = kmalloc(sizeof(struct p9_server_fid), GFP_KERNEL);
	if (!fid)
		return ERR_PTR(-ENOMEM);
	fid->fid = fid_val;
	fid->uid = s->uid;
	fid->filp = NULL;
	fid->path = *path;

	rb_link_node(&fid->node, parent, node);
	rb_insert_color(&fid->node, &s->fids);
    p9s_debug("fid : %d created\n", fid_val);

	return fid;
}

static inline void iov_iter_clone(struct iov_iter *dst, struct iov_iter *src)
{
	memcpy(dst, src, sizeof(struct iov_iter));
}

static int gen_qid(struct path *path, struct p9_qid *qid, struct kstat *st)
{
	int err;
	struct kstat _st;

	if (!st)
		st = &_st;

	err = vfs_getattr(path, st);
	if (err)
		return err;

	/* TODO: incomplete types */
	qid->version = st->mtime.tv_sec;
	qid->path = st->ino;
	qid->type = P9_QTFILE;

	if (S_ISDIR(st->mode))
		qid->type |= P9_QTDIR;

	if (S_ISLNK(st->mode))
		qid->type |= P9_QTSYMLINK;

	return 0;
}

/* 9p operation functions */

static int p9_op_version(struct p9_server *s, struct p9_fcall *in,
						 struct p9_fcall *out)
{
	u32 msize;
	char *version;

	p9pdu_readf(in, "ds", &msize, &version);

	if (!strcmp(version, "9P2000.L"))
		p9pdu_writef(out, "ds", msize, version);
	else
		p9pdu_writef(out, "ds", msize, "unknown");

	kfree(version);
	return 0;
}
// TODO: uname, aname, uid, afid
static int p9_op_attach(struct p9_server *s, struct p9_fcall *in,
						struct p9_fcall *out)
{
	int err;
	char *uname, *aname;
	struct p9_qid qid;
	struct p9_server_fid *fid;
	u32 fid_val, afid, uid;

	p9pdu_readf(in, "ddssd", &fid_val, &afid,
				&uname, &aname, &uid);
	kfree(uname);
	kfree(aname);

	s->uid = uid;
	fid = lookup_fid(s, fid_val);
	if (IS_ERR(fid)) {
		fid = new_fid(s, fid_val, &s->root);
		if (IS_ERR(fid))
			return PTR_ERR(fid);
	}

	err = gen_qid(&fid->path, &qid, NULL);
	if (err)
		return err;

	p9pdu_writef(out, "Q", &qid);
	return 0;
}
// TODO: request_mask
static int p9_op_getattr(struct p9_server *s, struct p9_fcall *in,
						 struct p9_fcall *out)
{
	int err;
	u32 fid_val;
	u64 request_mask;
	struct p9_server_fid *fid;
	struct kstat st;
	struct p9_qid qid;

	p9pdu_readf(in, "dq", &fid_val, &request_mask);

	fid = lookup_fid(s, fid_val);
	if (IS_ERR(fid))
		return PTR_ERR(fid);

	err = gen_qid(&fid->path, &qid, &st);
	if (err)
		return err;

	p9pdu_writef(out, "qQdugqqqqqqqqqqqqqqq",
		P9_STATS_BASIC, &qid, st.mode, st.uid, st.gid,
		st.nlink, st.rdev, st.size, st.blksize, st.blocks,
		st.atime.tv_sec, st.atime.tv_nsec,
		st.mtime.tv_sec, st.mtime.tv_nsec,
		st.ctime.tv_sec, st.ctime.tv_nsec,
		0, 0, 0, 0);

	return 0;
}

static int p9_op_clunk(struct p9_server *s, struct p9_fcall *in,
					   struct p9_fcall *out)
{
	u32 fid_val;
	struct p9_server_fid *fid;

	p9pdu_readf(in, "d", &fid_val);
    p9s_debug("destroy fid : %d\n", fid_val);
	fid = lookup_fid(s, fid_val);
	if (IS_ERR(fid))
		return 0;

	if (!IS_ERR_OR_NULL(fid->filp))
		filp_close(fid->filp, NULL);

	rb_erase(&fid->node, &s->fids);
	kfree(fid);
    p9s_debug("fid : %d destroyed\n ", fid_val);
	return 0;
}
/*
	http://man.cat-v.org/plan_9/5/walk
 */
static int p9_op_walk(struct p9_server *s, struct p9_fcall *in,
					  struct p9_fcall *out)
{
	int err;
	size_t t;
	u16 nwqid, nwname;
	u32 fid_val, newfid_val;
	char * name;
	struct p9_qid qid;
	struct p9_server_fid *fid, *newfid;
	struct path new_path;

	p9pdu_readf(in, "ddw", &fid_val, &newfid_val, &nwname);

	/* Get the indicated fid. */
	fid = lookup_fid(s, fid_val);
	if (IS_ERR(fid))
		return PTR_ERR(fid);

	/* Check if the newfid already exists. */
	newfid = lookup_fid(s, newfid_val);
	if (!IS_ERR(newfid) && newfid_val != fid_val)
		return -EEXIST;

	new_path = fid->path;
	nwqid = 0;
	out->size += sizeof(u16);

	if (nwname) {
		for (; nwqid < nwname; nwqid++) {
			p9pdu_readf(in, "s", &name);

			/* ".." is not allowed. */
			if (name[0] == '.' && name[1] == '.' && name[2] =='\0')
				break;

			// TODO: lock may be needed
			new_path.dentry = lookup_one_len(name, fid->path.dentry, strlen(name));
			kfree(name);

			if (IS_ERR(new_path.dentry)) {
				err = PTR_ERR(new_path.dentry);
				break;
			} else if (d_really_is_negative(new_path.dentry)) {
				err = -ENOENT;
				break;
			}

			err = gen_qid(&new_path, &qid, NULL);
			if (err)
				break;

			// TODO: verify if it's valid
			dput(new_path.dentry);

			p9pdu_writef(out, "Q", &qid);
		}

		if (!nwqid)
			return err;
	} else {
		/* If nwname is 0, it's equivalent to walking to the current directory. */
		err = gen_qid(&new_path, &qid, NULL);
		if (err)
			return err;

		p9pdu_writef(out, "Q", &qid);
	}

	if (fid_val == newfid_val) {
		fid->path = new_path;
	} else {
		newfid = new_fid(s, newfid_val, &new_path);
		if (IS_ERR(newfid))
			return PTR_ERR(newfid);
	}

	t = out->size;
	out->size = P9_PDU_HDR_LEN;
	p9pdu_writef(out, "w", nwqid);
	out->size = t;

	return 0;
}

static int p9_op_statfs(struct p9_server *s, struct p9_fcall *in,
						struct p9_fcall *out)
{
	int err;
	u64 fsid;
	u32 fid_val;
	struct p9_server_fid *fid;
	struct kstatfs st;

	p9pdu_readf(in, "d", &fid_val);

	fid = lookup_fid(s, fid_val);
	if (IS_ERR(fid))
		return PTR_ERR(fid);

	err = vfs_statfs(&fid->path, &st);
	if (err)
		return err;

	/* FIXME!! f_blocks needs update based on client msize */
	fsid = (unsigned int) st.f_fsid.val[0] |
		(unsigned long long)st.f_fsid.val[1] << 32;

	p9pdu_writef(out, "ddqqqqqqd", st.f_type,
			     st.f_bsize, st.f_blocks, st.f_bfree, st.f_bavail,
			     st.f_files, st.f_ffree, fsid, st.f_namelen);

	return 0;
}

/*
 * FIXME!! Need to map to protocol independent value. Upstream
 * 9p also have the same BUG
 */
static int build_openflags(int flags)
{
	flags &= ~(O_NOCTTY | FASYNC | O_CREAT | O_DIRECT);
	flags |= O_NOFOLLOW;
	return flags;
}

static int p9_op_open(struct p9_server *s, struct p9_fcall *in,
					  struct p9_fcall *out)
{
	int err;
	u32 fid_val, flags;
	struct p9_qid qid;
	struct p9_server_fid *fid;

	p9pdu_readf(in, "dd", &fid_val, &flags);

	fid = lookup_fid(s, fid_val);

	if (IS_ERR(fid))
		return PTR_ERR(fid);
	else if (fid->filp)	// TODO: verify if being error is also considered busy
		return -EBUSY;

	err = gen_qid(&fid->path, &qid, NULL);

	if (err)
		return err;

	fid->filp = dentry_open(&fid->path, build_openflags(flags), current_cred());

	if (IS_ERR(fid->filp)) {
		fid->filp = NULL;
		return PTR_ERR(fid->filp);
	}

	/* FIXME!! need ot send proper iounit  */
	p9pdu_writef(out, "Qd", &qid, 0L);

	return 0;
}

static int p9_op_create(struct p9_server *s, struct p9_fcall *in,
						struct p9_fcall *out)
{
	int err;
	char *name;
	u32 dfid_val, flags, mode, gid;
	struct p9_qid qid;
	struct p9_server_fid *dfid;
	struct path new_path;
	struct file *new_filp;

	p9pdu_readf(in, "d", &dfid_val);

	dfid = lookup_fid(s, dfid_val);

	if (IS_ERR(dfid))
		return PTR_ERR(dfid);
	else if (dfid->filp)
		return -EBUSY;

	p9pdu_readf(in, "sddd", &name, &flags, &mode, &gid);

	new_path.mnt = dfid->path.mnt;
	new_path.dentry = lookup_one_len(name, dfid->path.dentry, strlen(name));

	kfree(name);

	if (IS_ERR(new_path.dentry))
		return PTR_ERR(new_path.dentry);
	else if (d_really_is_positive(new_path.dentry)) {
		printk(KERN_NOTICE "create: postive dentry!\n");
		return -EEXIST;
	}

	err = vfs_create(dfid->path.dentry->d_inode, new_path.dentry, 
					 mode, build_openflags(flags) & O_EXCL);
	if (err)
		return err;

	new_filp = dentry_open(&new_path, build_openflags(flags) | O_CREAT, current_cred());
	if (IS_ERR(new_filp))
		return PTR_ERR(new_filp);

	err = gen_qid(&new_path, &qid, NULL);
	if (err)
		goto err;

	dfid->path = new_path;
	dfid->filp = new_filp;

	p9pdu_writef(out, "Qd", &qid, 0L);
	return 0;
err:
	filp_close(new_filp, NULL);
	return err;
}

struct p9_readdir_ctx {
	size_t i, count;
	int err;
	bool is_root;

	struct dir_context ctx;
	struct path *parent;
	struct p9_fcall *out;

	struct {
		struct p9_qid qid;
		const char *name;
		unsigned int d_type;
	} prev;
};

/*
 *	The callback function from iterate_dir.
 *
 *	Note: returning non-zero terminates the executing of iterate_dir.
 *		  However, iterate_dir will still return zero.
 *
 *	Note: weird logic: the offset is of the previous element. So we
 *		  deal with the previous element in each iteration.
 */

static int p9_readdir_cb(struct dir_context *ctx, const char *name, int namlen,
						 loff_t offset, u64 ino, unsigned int d_type)
{
	size_t write_len;
	struct path path;
	struct p9_readdir_ctx *_ctx = container_of(ctx, struct p9_readdir_ctx, ctx);

	write_len = sizeof(u8) + 	// qid.type. Not using sizeof(struct p9_qid) because of the size is padded.
				sizeof(u32) + 	// qid.version
				sizeof(u64) + 	// qid.path
				sizeof(u64) + 	// offset
				sizeof(u8) + 	// d_type
				sizeof(u16) + 	// name.len
				namlen;			// name

	// If writting this dirent would cause an overflow, terminate iterate_dir.
	if (_ctx->i + write_len > _ctx->count)
		return 1;

	// Writting the previous element with current offset
	if (_ctx->i)
		p9pdu_writef(_ctx->out, "Qqbs", &_ctx->prev.qid, (u64) offset, _ctx->prev.d_type, _ctx->prev.name);

	/* Prepare the dirent for the next iteration. */

	path.mnt = _ctx->parent->mnt;

	// lookup_one_len doesn't allow the lookup of "." and "..". We have to do it ourselves.
	if (namlen == 1 && name[0] == '.')
		path.dentry = _ctx->parent->dentry;
	else if (namlen == 2 && name[0] == '.' && name[1] == '.')
		// No ".." allowed on the mount root
		path.dentry = _ctx->is_root ? _ctx->parent->dentry : _ctx->parent->dentry->d_parent;
	else
		path.dentry = lookup_one_len(name, _ctx->parent->dentry, namlen);

	if (IS_ERR(path.dentry)) {
		_ctx->err = PTR_ERR(path.dentry);
		goto out;
	} else if (d_really_is_negative(path.dentry)) {
		_ctx->err = -ENOENT;
		goto out;
	}

	_ctx->err = gen_qid(&path, &_ctx->prev.qid, NULL);
	if (_ctx->err)
		goto out;

	_ctx->prev.name = name;
	_ctx->prev.d_type = d_type;

	_ctx->i += write_len;
out:
	return _ctx->err;
}

static int p9_op_readdir(struct p9_server *s, struct p9_fcall *in,
						 struct p9_fcall *out)
{
	int err;
	u32 dfid_val, count;
	u64 offset;
	struct p9_server_fid *dfid;
	struct p9_readdir_ctx _ctx = {
		.ctx.actor = p9_readdir_cb
	};

	p9pdu_readf(in, "dqd", &dfid_val, &offset, &count);

	dfid = lookup_fid(s, dfid_val);
	if (IS_ERR(dfid))
		return PTR_ERR(dfid);

	if (IS_ERR_OR_NULL(dfid->filp))
		return -EBADF;

	err = vfs_llseek(dfid->filp, offset, SEEK_SET);
	if (err < 0)
		return err;

	_ctx.parent = &dfid->path;
	_ctx.out = out;
	_ctx.i = 0;
	_ctx.count = count;
	_ctx.err = 0;
	_ctx.is_root = (dfid->path.dentry == s->root.dentry);

	out->size += sizeof(u32);	// Make room for count

	err = iterate_dir(dfid->filp, &_ctx.ctx);
	if (err)
		return err;
	if (_ctx.err)
		return _ctx.err;

	// Write the last element
	if (_ctx.i)
		p9pdu_writef(out, "Qqbs", &_ctx.prev.qid, (u64) _ctx.ctx.pos, _ctx.prev.d_type, _ctx.prev.name);

	out->size = P9_PDU_HDR_LEN;
	p9pdu_writef(out, "d", _ctx.i);	// Total bytes written
	out->size += _ctx.i;

	return 0;
}

static int p9_op_read(struct p9_server *s, struct p9_fcall *in,
					  struct p9_fcall *out)
{
	u32 fid_val, count;
	u64 offset;
	ssize_t len;
	struct p9_server_fid *fid;
	mm_segment_t fs;

	p9pdu_readf(in, "dqd", &fid_val, &offset, &count);

	fid = lookup_fid(s, fid_val);
	if (IS_ERR(fid))
		return PTR_ERR(fid);

	if (IS_ERR_OR_NULL(fid->filp))
		return -EBADF;

	out->size += sizeof(u32);

	if (count + out->size > out->capacity)
		count = out->capacity - out->size;

	fs = get_fs();
	set_fs(KERNEL_DS);
	len = vfs_read(fid->filp, out->sdata + out->size, count, &offset);
	set_fs(fs);

	if (len < 0)
		return len;

	out->size = P9_PDU_HDR_LEN;
	p9pdu_writef(out, "d", (u32) len);
	out->size += len;

	return 0;
}

static int p9_op_readv(struct p9_server *s, struct p9_fcall *in,
					   struct p9_fcall *out, struct iov_iter *data)
{
	u32 fid_val, count;
	u64 offset;
	ssize_t len;
	struct p9_server_fid *fid;
	mm_segment_t fs;

	p9pdu_readf(in, "dqd", &fid_val, &offset, &count);

	fid = lookup_fid(s, fid_val);
	if (IS_ERR(fid))
		return PTR_ERR(fid);

	if (IS_ERR_OR_NULL(fid->filp))
		return -EBADF;

	if (data->count > count)
		data->count = count;

	fs = get_fs();
	set_fs(KERNEL_DS);
	len = vfs_iter_read(fid->filp, data, &offset);
	set_fs(fs);

	if (len < 0)
		return len;

	p9pdu_writef(out, "d", (u32) len);
	out->size += len;

	return 0;
}

#define ATTR_MASK	127

static int p9_op_setattr(struct p9_server *s, struct p9_fcall *in,
						 struct p9_fcall *out)
{
	int err = 0; /* TODO: remove */
	u32 fid_val;
	struct p9_server_fid *fid;
	struct p9_iattr_dotl p9attr;

	p9pdu_readf(in, "dddugqqqqq", &fid_val, 
				&p9attr.valid, &p9attr.mode,
				&p9attr.uid, &p9attr.gid, &p9attr.size,
				&p9attr.atime_sec, &p9attr.atime_nsec,
				&p9attr.mtime_sec, &p9attr.mtime_nsec);

	fid = lookup_fid(s, fid_val);
	if (IS_ERR(fid))
		return PTR_ERR(fid);
/*
	if (p9attr.valid & ATTR_MODE) {
		err = chmod(fid->path, p9attr.mode);
		if (err < 0)
			return err;
	}

	if (p9attr.valid & (ATTR_ATIME | ATTR_MTIME)) {
		struct timespec times[2];

		if (p9attr.valid & ATTR_ATIME) {
			if (p9attr.valid & ATTR_ATIME_SET) {
				times[0].tv_sec = p9attr.atime_sec;
				times[0].tv_nsec = p9attr.atime_nsec;
			} else
				times[0].tv_nsec = UTIME_NOW;
		} else
			times[0].tv_nsec = UTIME_OMIT;

		if (p9attr.valid & ATTR_MTIME) {
			if (p9attr.valid & ATTR_MTIME_SET) {
				times[1].tv_sec = p9attr.mtime_sec;
				times[1].tv_nsec = p9attr.mtime_nsec;
			} else
				times[1].tv_nsec = UTIME_NOW;
		} else
			times[1].tv_nsec = UTIME_OMIT;

		err = utimensat(-1, fid->path, times, AT_SYMLINK_NOFOLLOW);
		if (err < 0)
			return err;
	}
*/	/*
	 * If the only valid entry in iattr is ctime we can call
	 * chown(-1,-1) to update the ctime of the file
	 */
/*	if ((p9attr.valid & (ATTR_UID | ATTR_GID)) ||
	    ((p9attr.valid & ATTR_CTIME)
	     && !((p9attr.valid & ATTR_MASK) & ~ATTR_CTIME))) {
		if (!(p9attr.valid & ATTR_UID))
			p9attr.uid = KUIDT_INIT(-1);

		if (!(p9attr.valid & ATTR_GID))
			p9attr.gid = KGIDT_INIT(-1);

		err = lchown(fid->path, __kuid_val(p9attr.uid),
				__kgid_val(p9attr.gid));
		if (err < 0)
			return err;
	}
*/
	if (p9attr.valid & ATTR_SIZE) {
		err = vfs_truncate(&fid->path, p9attr.size);

		if (err < 0)
			return err;
	}

	return 0;
}

static int p9_op_write(struct p9_server *s, struct p9_fcall *in,
					   struct p9_fcall *out)
{
	u64 offset;
	u32 fid_val, count;
	ssize_t len;
	struct p9_server_fid *fid;
	mm_segment_t fs;

	p9pdu_readf(in, "dqd", &fid_val, &offset, &count);

	fid = lookup_fid(s, fid_val);
	if (IS_ERR(fid))
		return PTR_ERR(fid);

	if (IS_ERR_OR_NULL(fid->filp))
		return -EBADF;

	fs = get_fs();
	set_fs(KERNEL_DS);
	len = vfs_write(fid->filp, in->sdata + in->offset, count, &offset);
	set_fs(fs);

	if (len < 0)
		return len;

	p9pdu_writef(out, "d", (u32) len);
	return 0;
}

static int p9_op_writev(struct p9_server *s, struct p9_fcall *in,
						struct p9_fcall *out, struct iov_iter *data)
{
	u64 offset;
	u32 fid_val, count;
	ssize_t len;
	struct p9_server_fid *fid;

	p9pdu_readf(in, "dqd", &fid_val, &offset, &count);

	fid = lookup_fid(s, fid_val);
	if (IS_ERR(fid))
		return PTR_ERR(fid);

	if (IS_ERR_OR_NULL(fid->filp))
		return -EBADF;

	if (data->count > count)
		data->count = count;

	len = vfs_iter_write(fid->filp, data, &offset);

	if (len < 0)
		return len;

	p9pdu_writef(out, "d", (u32) len);
	return 0;
}

static int p9_op_remove(struct p9_server *s, struct p9_fcall *in,
						struct p9_fcall *out)
{
	u32 fid_val;
	struct p9_server_fid *fid;
	struct dentry *dentry;
	int err;

	p9pdu_readf(in, "d", &fid_val);

	fid = lookup_fid(s, fid_val);
	if (IS_ERR(fid))
		return PTR_ERR(fid);

	dentry = fid->path.dentry;

	// TODO: null check
	if (d_really_is_negative(dentry))
		return -ENOENT;

	if (S_ISDIR(dentry->d_inode->i_mode))
		err = vfs_rmdir(dentry->d_parent->d_inode, dentry);
	else
		err = vfs_unlink(dentry->d_parent->d_inode, dentry, NULL);

	rb_erase(&fid->node, &s->fids);
	return err;
}

// TODO: resolve this hack
extern int vfs_path_lookup(struct dentry *dentry, struct vfsmount *mnt,
		    const char *name, unsigned int flags,
		    struct path *path);

static int p9_op_rename(struct p9_server *s, struct p9_fcall *in,
						struct p9_fcall *out)
{
	int err;
	u32 fid_val, newfid_val;
	char *path;
	struct p9_server_fid *fid, *newfid;
	struct dentry *old_dentry, *new_dentry;
	struct path new_path;

	p9pdu_readf(in, "d", &fid_val);

	fid = lookup_fid(s, fid_val);

	if (IS_ERR(fid))
		return PTR_ERR(fid);

	p9pdu_readf(in, "ds", &newfid_val, &path);

	err = vfs_path_lookup(fid->path.dentry, fid->path.mnt, path, LOOKUP_RENAME_TARGET, &new_path);
	if (err < 0) {
		kfree(path);
		return err;
	}

	// TODO: security: new dir under the root

	newfid = new_fid(s, newfid_val, &new_path);

	kfree(path);

	if (IS_ERR(newfid))
		return PTR_ERR(newfid);

	old_dentry = fid->path.dentry;
	new_dentry = newfid->path.dentry;

	return vfs_rename(old_dentry->d_parent->d_inode, old_dentry,
					  new_dentry->d_parent->d_inode, new_dentry,
					  NULL, 0);
}
// TODO: gid
static int p9_op_mkdir(struct p9_server *s, struct p9_fcall *in,
					   struct p9_fcall *out)
{
	int err;
	u32 dfid_val, mode, gid;
	char *name;
	struct p9_qid qid;
	struct p9_server_fid *dfid;
	struct path new_path;

	p9pdu_readf(in, "d", &dfid_val);

	dfid = lookup_fid(s, dfid_val);

	if (IS_ERR(dfid))
		return PTR_ERR(dfid);

	p9pdu_readf(in, "sdd", &name, &mode, &gid);

	new_path.mnt = dfid->path.mnt;
	new_path.dentry = lookup_one_len(name, dfid->path.dentry, strlen(name));

	kfree(name);

	if (IS_ERR(new_path.dentry)) {
		return PTR_ERR(new_path.dentry);
	} else if (d_really_is_positive(new_path.dentry)) {
		printk(KERN_NOTICE "mkdir: postive dentry!\n");
		return -EEXIST;
	}

	// TODO: verify dfid's inode is valid

	err = vfs_mkdir(dfid->path.dentry->d_inode, new_path.dentry, mode);
	if (err < 0)
		return err;

	err = gen_qid(&new_path, &qid, NULL);
	if (err)
		return err;

	dfid->path = new_path;

	p9pdu_writef(out, "Qd", &qid, 0L);

	return 0;
}

static int p9_op_symlink(struct p9_server *s, struct p9_fcall *in,
						 struct p9_fcall *out)
{
	int err;
	u32 fid_val, gid;
	struct p9_qid qid;
	struct p9_server_fid *fid;
	char *name, *dst;
	struct path symlink_path;

	p9pdu_readf(in, "d", &fid_val);

	fid = lookup_fid(s, fid_val);

	if (IS_ERR(fid))
		return PTR_ERR(fid);

	p9pdu_readf(in, "ssd", &name, &dst, &gid);

	symlink_path.mnt = fid->path.mnt;
	symlink_path.dentry = lookup_one_len(name, fid->path.dentry, strlen(name));
	kfree(name);

	if (d_really_is_positive(symlink_path.dentry)) {
		kfree(dst);
		return -EEXIST;
	}

	// TODO: security: symlink target must be strictly under the root

	err = vfs_symlink(fid->path.dentry->d_inode, symlink_path.dentry, dst);

	kfree(dst);

	if (err < 0)
		return err;

	err = gen_qid(&symlink_path, &qid, NULL);
	if (err)
		return err;

	p9pdu_writef(out, "Q", &qid);

	return 0;
}

static int p9_op_link(struct p9_server *s, struct p9_fcall *in,
					  struct p9_fcall *out)
{
	char *name;
	u32 dfid_val, fid_val;
	struct p9_server_fid *dfid, *fid;	// fid is the file to be hard-linked. dfid is the directory where the created hard link lies.
	struct dentry *new_dentry;

	p9pdu_readf(in, "dd", &dfid_val, &fid_val);

	fid = lookup_fid(s, fid_val);

	if (IS_ERR(fid))
		return PTR_ERR(fid);

	dfid = lookup_fid(s, dfid_val);

	if (IS_ERR(dfid))
		return PTR_ERR(dfid);

	p9pdu_readf(in, "s", &name);

	new_dentry = lookup_one_len(name, dfid->path.dentry, strlen(name));

	kfree(name);

	if (IS_ERR(new_dentry)) {
		return PTR_ERR(new_dentry);
	} else if (d_really_is_positive(new_dentry)) {
		printk(KERN_NOTICE "link: postive dentry!\n");
		return -EEXIST;
	}

	// TODO: make sure dfid dentry is positive

	return vfs_link(fid->path.dentry, dfid->path.dentry->d_inode, new_dentry, NULL);
}
// TODO: put path
static int p9_op_readlink(struct p9_server *s, struct p9_fcall *in,
						  struct p9_fcall *out)
{
	int err;
	char *path;
	u32 fid_val;
	struct p9_server_fid *fid;
	struct dentry *dentry;
	struct inode *inode;

	p9pdu_readf(in, "d", &fid_val);

	fid = lookup_fid(s, fid_val);

	if (IS_ERR(fid))
		return PTR_ERR(fid);

	dentry = fid->path.dentry;
	inode = d_backing_inode(dentry);
	path = kmalloc(PATH_MAX, GFP_KERNEL);

	// TODO: security check
	err = inode->i_op->readlink(dentry, path, PATH_MAX);

	if (!err)
		p9pdu_writef(out, "s", path);

	kfree(path);

	return err;
}

static int p9_op_fsync(struct p9_server *s, struct p9_fcall *in,
					   struct p9_fcall *out)
{
	int err = -EBADFD;
	u32 fid_val, datasync;
	struct p9_server_fid *fid;

	p9pdu_readf(in, "dd", &fid_val, &datasync);

	fid = lookup_fid(s, fid_val);
	if (IS_ERR(fid))
		return PTR_ERR(fid);

	// TODO: verify that filp can't be error

	if (!IS_ERR_OR_NULL(fid->filp))
		err = vfs_fsync(fid->filp, datasync);

	return err;
}

// TODO: gid
static int p9_op_mknod(struct p9_server *s, struct p9_fcall *in,
					   struct p9_fcall *out)
{
	int err;
	char *name;
	u32 dfid_val, mode, major, minor, gid;
	struct p9_qid qid;
	struct p9_server_fid *dfid;
	struct path new_path;

	p9pdu_readf(in, "d", &dfid_val);

	dfid = lookup_fid(s, dfid_val);

	if (IS_ERR(dfid))
		return PTR_ERR(dfid);

	p9pdu_readf(in, "sdddd", &name, &mode, &major, &minor, &gid);

	new_path.mnt = dfid->path.mnt;
	new_path.dentry = lookup_one_len(name, dfid->path.dentry, strlen(name));

	kfree(name);

	if (IS_ERR(new_path.dentry)) {
		return PTR_ERR(new_path.dentry);
	} else if (d_really_is_positive(new_path.dentry)) {
		printk(KERN_NOTICE "mknod: postive dentry!\n");
		return -EEXIST;
	}

	err = vfs_mknod(dfid->path.dentry->d_inode, new_path.dentry, mode, MKDEV(major, minor));

	if (err < 0)
		return err;

	// TODO: need chmod?

	err = gen_qid(&new_path, &qid, NULL);
	if (err)
		return err;

	p9pdu_writef(out, "Q", &qid);

	return 0;
}

static int p9_op_lock(struct p9_server *s, struct p9_fcall *in,
					  struct p9_fcall *out)
{
	u32 fid_val;
	struct p9_flock flock;

	p9pdu_readf(in, "dbdqqds", &fid_val, &flock.type,
			    &flock.flags, &flock.start, &flock.length,
			    &flock.proc_id, &flock.client_id);

	kfree(flock.client_id);

	/* Just return success */
	p9pdu_writef(out, "d", (u8) P9_LOCK_SUCCESS);
	return 0;
}

static int p9_op_getlock(struct p9_server *s, struct p9_fcall *in,
						 struct p9_fcall *out)
{
	u32 fid_val;
	struct p9_getlock glock;

	p9pdu_readf(in, "dbqqds", &fid_val, &glock.type,
				&glock.start, &glock.length, &glock.proc_id,
				&glock.client_id);

	/* Just return success */
	glock.type = F_UNLCK;
	p9pdu_writef(out, "bqqds", glock.type,
				 glock.start, glock.length, glock.proc_id,
				 glock.client_id);

	kfree(glock.client_id);
	return 0;
}

static int p9_op_flush(struct p9_server *s, struct p9_fcall *in,
					   struct p9_fcall *out)
{
	u16 tag, oldtag;

	p9pdu_readf(in, "ww", &tag, &oldtag);
	p9pdu_writef(out, "w", tag);

	return 0;
}

typedef int p9_server_op(struct p9_server *s, struct p9_fcall *in,
			struct p9_fcall *out);

static p9_server_op *p9_ops [] = {
//	[P9_TLERROR]      = p9_op_error,	// Not used
	[P9_TSTATFS]      = p9_op_statfs,
	[P9_TLOPEN]       = p9_op_open,
	[P9_TLCREATE]     = p9_op_create,
	[P9_TSYMLINK]     = p9_op_symlink,
	[P9_TMKNOD]       = p9_op_mknod,
	[P9_TRENAME]      = p9_op_rename,
	[P9_TREADLINK]    = p9_op_readlink,
	[P9_TGETATTR]     = p9_op_getattr,
	[P9_TSETATTR]     = p9_op_setattr,
//	[P9_TXATTRWALK]   = p9_op_xattrwalk,	// Not implemented
//	[P9_TXATTRCREATE] = p9_op_xattrcreate,	// Not implemented
	[P9_TREADDIR]     = p9_op_readdir,
	[P9_TFSYNC]       = p9_op_fsync,
	[P9_TLOCK]        = p9_op_lock,
	[P9_TGETLOCK]     = p9_op_getlock,
	[P9_TLINK]        = p9_op_link,
	[P9_TMKDIR]       = p9_op_mkdir,
//	[P9_TRENAMEAT]    = p9_op_renameat,	// Not supported. No easy way to implement besides syscalls
//	[P9_TUNLINKAT]    = p9_op_unlinkat,	// Not supported. No easy way to implement besides syscalls
	[P9_TVERSION]     = p9_op_version,
//	[P9_TAUTH]        = p9_op_auth,	// Not implemented
	[P9_TATTACH]      = p9_op_attach,
//	[P9_TERROR]       = p9_op_error,		// Not used
	[P9_TFLUSH]       = p9_op_flush,
	[P9_TWALK]        = p9_op_walk,
//	[P9_TOPEN]        = p9_op_open,	// Not supported in 9P2000.L
//	[P9_TCREATE]      = p9_op_create,	// Not supported in 9P2000.L
	[P9_TREAD]        = p9_op_read,
	[P9_TWRITE]       = p9_op_write,
	[P9_TCLUNK]       = p9_op_clunk,
	[P9_TREMOVE]      = p9_op_remove,
//	[P9_TSTAT]        = p9_op_stat,	// Not implemented
//	[P9_TWSTAT]       = p9_op_wstat,	// Not implemented
};

static const char *translate [] = {
	[P9_TLERROR]      = "error",
	[P9_TSTATFS]      = "statfs",
	[P9_TLOPEN]       = "open",
	[P9_TLCREATE]     = "create",
	[P9_TSYMLINK]     = "symlink",
	[P9_TMKNOD]       = "mknod",
	[P9_TRENAME]      = "rename",
	[P9_TREADLINK]    = "readlink",
	[P9_TGETATTR]     = "getattr",
	[P9_TSETATTR]     = "setattr",
	[P9_TXATTRWALK]   = "xattrwalk",
	[P9_TXATTRCREATE] = "xattrcreate",
	[P9_TREADDIR]     = "readdir",
	[P9_TFSYNC]       = "fsync",
	[P9_TLOCK]        = "lock",
	[P9_TGETLOCK]     = "getlock",
	[P9_TLINK]        = "link",
	[P9_TMKDIR]       = "mkdir",
	[P9_TRENAMEAT]    = "renameat",
	[P9_TUNLINKAT]    = "unlinkat",
	[P9_TVERSION]     = "version",
	[P9_TAUTH]        = "auth",
	[P9_TATTACH]      = "attach",
	[P9_TERROR]       = "error",
	[P9_TFLUSH]       = "flush",
	[P9_TWALK]        = "walk",
	[P9_TOPEN]        = "open",
	[P9_TCREATE]      = "create",
	[P9_TREAD]        = "read",
	[P9_TWRITE]       = "write",
	[P9_TCLUNK]       = "clunk",
	[P9_TREMOVE]      = "remove",
	[P9_TSTAT]        = "stat",
	[P9_TWSTAT]       = "wstat",
};

struct p9_header {
	uint32_t size;
	uint8_t id;
	uint16_t tag;
} __attribute__((packed));

struct p9_io_header {
	uint32_t size;
	uint8_t id;
	uint16_t tag;
	uint32_t fid;
	uint64_t offset;
	uint32_t count;
} __attribute__((packed));

static struct p9_fcall *new_pdu(size_t size)
{
	struct p9_fcall *pdu;

	pdu = kmalloc(sizeof(struct p9_fcall) + size, GFP_KERNEL);
	pdu->size = 0;	// write offset
	pdu->offset = 0;	// read offset
	pdu->capacity = size;
	pdu->sdata = (void *)pdu + sizeof(struct p9_fcall);	// Make the data area right after the pdu structure

	return pdu;
}

static size_t pdu_fill(struct p9_fcall *pdu, struct iov_iter *from, size_t size)
{
	size_t ret, len;

	len = min(pdu->capacity - pdu->size, size);
	ret = copy_from_iter(&pdu->sdata[pdu->size], len, from);

	pdu->size += ret;
	return size - ret;
}

void do_9p_request(struct p9_server *s, struct iov_iter *req, struct iov_iter *resp)
{
	int err = -EOPNOTSUPP;
	u8 cmd;
	struct iov_iter data;
	struct p9_fcall *in, *out;
	struct p9_io_header *hdr;	// Assume the operation is an IO operation to save additional copy_from_iter.

	in = new_pdu(req->count);
	out = new_pdu(resp->count);

	pdu_fill(in, req, sizeof(struct p9_io_header));
	hdr = (struct p9_io_header *)in->sdata;

	in->offset = out->size = sizeof(struct p9_header);
	in->tag = out->tag = hdr->tag;
	in->id = cmd = hdr->id;
	out->id = hdr->id + 1;

	printk(KERN_NOTICE "do_9p_request: %s! %d\n", translate[cmd], in->tag);

	if (cmd < ARRAY_SIZE(p9_ops) && p9_ops[cmd]) {
		if (cmd == P9_TREAD || cmd == P9_TWRITE) {
			/* Do zero-copy for large IO */
			if (hdr->count > 1024) {
				if (cmd == P9_TREAD) {
					iov_iter_clone(&data, resp);
					iov_iter_advance(&data, sizeof(struct p9_header) + sizeof(u32));
					resp->count = sizeof(struct p9_header) + sizeof(u32);	// Hack

					err = p9_op_readv(s, in, out, &data);
				} else
					err = p9_op_writev(s, in, out, req);
			} else {
				if (cmd == P9_TWRITE) {
					pdu_fill(in, req, hdr->count);
					err = p9_op_write(s, in, out);
				} else
					err = p9_op_read(s, in, out);
			}
		} else {
			/* Copy the rest data */
			if (hdr->size > sizeof(struct p9_io_header))
				pdu_fill(in, req, hdr->size - sizeof(struct p9_io_header));

			err = p9_ops[cmd](s, in, out);
		}

		kfree(in);
	} else {
		if (cmd < ARRAY_SIZE(p9_ops))
			printk(KERN_WARNING "!!!not implemented: %s\n", translate[cmd]);
		else
			printk(KERN_WARNING "!!!cmd too large: %d\n", (u32) cmd);
	}

	if (err) {
		printk(KERN_ERR "9p request error: %d\n", err);
		/* Compose an error reply */
		out->size = 0;
		p9pdu_writef(out, "dbwd", 
			sizeof(struct p9_header) + sizeof(u32), 
			P9_RLERROR, out->tag, (u32) -err);
	} else {
		size_t t = out->size;
		out->size = 0;
		p9pdu_writef(out, "dbw", t, out->id, out->tag);
		out->size = t;
	}

	copy_to_iter(out->sdata, out->size, resp);
	kfree(out);
}

struct p9_server *p9_server_create(struct path *root)
{
	struct p9_server *s;

	printk(KERN_INFO "9p server create!\n");

	s = kmalloc(sizeof(struct p9_server), GFP_KERNEL);
	if (!s)
		return ERR_PTR(-ENOMEM);

	s->root = *root;
	s->fids = RB_ROOT;

	return s;
}

void p9_server_close(struct p9_server *s)
{
	if (!IS_ERR_OR_NULL(s))
		kfree(s);
}

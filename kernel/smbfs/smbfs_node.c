/*
 * Copyright (c) 2000-2001 Boris Popov
 * All rights reserved.
 *
 * Portions Copyright (C) 2001 - 2013 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    This product includes software developed by Boris Popov.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/time.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/malloc.h>
#include <sys/sysctl.h>
#include <sys/queue.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <libkern/OSAtomic.h>

#include <libkern/crypto/md5.h>

#include <sys/kauth.h>
#include <sys/paths.h>

#include <sys/smb_apple.h>
#include <sys/smb_byte_order.h>
#include <sys/mchain.h>
#include <sys/msfscc.h>
#include <netsmb/smb.h>
#include <netsmb/smb_2.h>
#include <netsmb/smb_rq.h>
#include <netsmb/smb_rq_2.h>
#include <netsmb/smb_conn.h>
#include <netsmb/smb_conn_2.h>
#include <netsmb/smb_subr.h>

#include <smbfs/smbfs.h>
#include <smbfs/smbfs_node.h>
#include <smbfs/smbfs_subr.h>
#include <smbfs/smbfs_subr_2.h>
#include <Triggers/triggers.h>
#include <smbclient/smbclient_internal.h>

#define	SMBFS_NOHASH(smp, hval)	(&(smp)->sm_hash[(hval) & (smp)->sm_hashlen])
#define	smbfs_hash_lock(smp)	(lck_mtx_lock((smp)->sm_hashlock))
#define	smbfs_hash_unlock(smp)	(lck_mtx_unlock((smp)->sm_hashlock))

extern vnop_t **smbfs_vnodeop_p;

MALLOC_DEFINE(M_SMBNODE, "SMBFS node", "SMBFS vnode private part");
MALLOC_DEFINE(M_SMBNODENAME, "SMBFS nname", "SMBFS node name");

#define	FNV_32_PRIME ((uint32_t) 0x01000193UL)
#define	FNV1_32_INIT ((uint32_t) 33554467UL)

#define isdigit(d) ((d) >= '0' && (d) <= '9')

/*
 * smbfs_build_path
 *
 * Build a path that starts from the root node and includes this node. May
 * want to remove the SMBFS_MAXPATHCOMP limit in the future. That would require
 * two passes through the loop.
 */
static int 
smbfs_build_path(char *path, struct smbnode *np, size_t maxlen)
{
    struct smbmount *smp = np->n_mount;
	struct smbnode  *npstack[SMBFS_MAXPATHCOMP]; 
	struct smbnode  **npp = &npstack[0]; 
	int i, error = 0;
    int lock_count = 0;
	struct smbnode  *lock_stack[SMBFS_MAXPATHCOMP];
	struct smbnode  **locked_npp = &lock_stack[0];
	
    /*
     * We hold sm_reclaim_lock to protect np->n_parent fields from a
     * race with smbfs_vnop_reclaim()/smbfs_ClearChildren() since we are 
     * walking all the parents up to the root vnode. Always lock 
     * sm_reclaim_lock first and then individual n_parent_rwlock next.
     * See <rdar://problem/15707521>.
     */
	lck_mtx_lock(&smp->sm_reclaim_lock);

    lck_rw_lock_shared(&np->n_parent_rwlock);
    *locked_npp++ = np;     /* Save node to be unlocked later */
    lock_count += 1;

    i = 0;
    while (np->n_parent) {
		if (i++ == SMBFS_MAXPATHCOMP) {
			error = ENAMETOOLONG;
            goto done;
        }
		*npp++ = np;

        np = np->n_parent;
        
        lck_rw_lock_shared(&np->n_parent_rwlock);
        *locked_npp++ = np;     /* Save node to be unlocked later */
        lock_count += 1;
	}

	while (i-- && !error) {
		np = *--npp;
		if (strlcat(path, "/", MAXPATHLEN) >= maxlen) {
			error = ENAMETOOLONG;
		}
        else {
            lck_rw_lock_shared(&np->n_name_rwlock);
			if (strlcat(path, (char *)np->n_name, maxlen) >= maxlen) {
				error = ENAMETOOLONG;
			}
            lck_rw_unlock_shared(&np->n_name_rwlock);
		}
	}
    
done:
    /* Unlock all the nodes */
    for (i = 0; i < lock_count; i++) {
        lck_rw_unlock_shared(&lock_stack[i]->n_parent_rwlock);
    }

    lck_mtx_unlock(&smp->sm_reclaim_lock);
	return error;
}

static void *
smbfs_trigger_get_mount_args(vnode_t vp, __unused vfs_context_t ctx, 
							 int *errp)
{
	struct mount_url_callargs *argsp;
	int	error = 0;
	int	length;
	char	*url, *mountOnPath;
	struct smbmount *smp = VTOSMB(vp)->n_mount;

	/*
	 * Allocate the args structure
	 */
	SMB_MALLOC(argsp, struct mount_url_callargs *, sizeof (*argsp), M_SMBFSDATA, M_WAITOK);

	/*
	 * Get the UID for which the mount should be done; it's the
	 * UID for which the mount containing the trigger was done,
	 * which might not be the UID for the process that triggered
	 * the mount.
	 */
	argsp->muc_uid = smp->sm_args.uid;

	/* 
	 * Create the URL
	 * 1. smb:
	 * 2. vnode's mount point from name
	 * 3. path from the root to this vnode.
	 * 4. URL must be less than MAXPATHLEN
	 *
	 * What should be the max length, for URL should it be the MAXPATHLEN
	 * plus the scheme.
	 */
	SMB_MALLOC(url, char *, MAXPATHLEN, M_SMBFSDATA, M_WAITOK | M_ZERO);
	strlcpy(url, "smb:", MAXPATHLEN);
	if (strlcat(url, vfs_statfs(vnode_mount(vp))->f_mntfromname, MAXPATHLEN) >= MAXPATHLEN) {
		error = ENAMETOOLONG;
	} else {
		error = smbfs_build_path(url, VTOSMB(vp), MAXPATHLEN);
	}
	if (error) {
		SMBERROR_LOCK(VTOSMB(vp), "%s: URL FAILED url = %s\n", VTOSMB(vp)->n_name, url);

		SMB_FREE(url, M_SMBFSDATA);
		SMB_FREE(argsp, M_SMBFSDATA);
		*errp = error;
		return (NULL);
	}
	
	/* Create the mount on path */
	SMB_MALLOC(mountOnPath, char *, MAXPATHLEN, M_SMBFSDATA, M_WAITOK | M_ZERO);
	length = MAXPATHLEN;
	/* This can fail sometimes, should we even bother with it? */
	error = vn_getpath(vp, mountOnPath, &length);
	if (error) {
		SMBERROR_LOCK(VTOSMB(vp), "%s: vn_getpath FAILED, using smbfs_build_path!\n", VTOSMB(vp)->n_name);
        
		if (strlcpy(mountOnPath, vfs_statfs(vnode_mount(vp))->f_mntonname, MAXPATHLEN) >= MAXPATHLEN) {
			error = ENAMETOOLONG;
		} else {
			error = smbfs_build_path(mountOnPath, VTOSMB(vp), MAXPATHLEN);	
		}
	}
	if (error) {
		SMBERROR_LOCK(VTOSMB(vp), "%s: Mount on name FAILED url = %s\n", VTOSMB(vp)->n_name, url);
        
		SMB_FREE(mountOnPath, M_SMBFSDATA);
		SMB_FREE(url, M_SMBFSDATA);
		SMB_FREE(argsp, M_SMBFSDATA);
		*errp = error;
		return (NULL);
	}
    
    SMBWARNING_LOCK(VTOSMB(vp), "%s: Triggering with URL = %s mountOnPath = %s\n",
                    VTOSMB(vp)->n_name, url, mountOnPath);

	argsp->muc_url = url;
	argsp->muc_mountpoint = mountOnPath;
	argsp->muc_opts = (smp->sm_args.altflags & SMBFS_MNT_SOFT) ? (char *)"soft" : (char *)"";
	*errp = 0;
	return (argsp);
}
	
static void
smbfs_trigger_rel_mount_args(void *data)
{
	struct mount_url_callargs *argsp = data;

	SMB_FREE(argsp->muc_url, M_SMBFSDATA);
	SMB_FREE(argsp->muc_mountpoint, M_SMBFSDATA);
	SMB_FREE(argsp, M_SMBFSDATA);
}

/*
 * See if this is one of those faked up symbolic link. This is Conrad and Steve
 * French method for storing and reading symlinks on Window Servers.
 *
 * The calling routine must hold a reference on the share
 *
 */
static int 
smb_check_for_windows_symlink(struct smb_share *share, struct smbnode *np, 
							  int *symlen, vfs_context_t context)
{
    uio_t uio = NULL;
    MD5_CTX md5;
    char m5b[SMB_SYMMD5LEN];
    uint32_t state[4];
    int len = 0;
    unsigned char *sb = NULL;
    unsigned char *cp;
    SMBFID fid = 0;
    int error, cerror;
    size_t read_size = 0; /* unused */

    SMB_MALLOC(sb, void *, (size_t) np->n_size, M_TEMP, M_WAITOK);
    if (sb == NULL) {
        error = ENOMEM;
        goto exit;
    }
    
    uio = uio_create(1, 0, UIO_SYSSPACE, UIO_READ);
    if (uio == NULL) {
        error = ENOMEM;
        goto exit;
    }

    uio_addiov(uio, CAST_USER_ADDR_T(sb), np->n_size);
    
    if (SSTOVC(share)->vc_flags & SMBV_SMB2) {
        /* SMB 2/3 */
        error = smbfs_smb_cmpd_create_read_close(share, np,
                                                 NULL, 0,
                                                 NULL, 0,
                                                 uio, &read_size,
                                                 NULL,
                                                 context);
    }
    else {
        /* SMB 1 */
        error = smbfs_tmpopen(share, np, SMB2_FILE_READ_DATA, &fid, context);
        if (error) {
            goto exit;
        }

        error = smb_smb_read(share, fid, uio, context);
        
        cerror = smbfs_tmpclose(share, np, fid, context);
        if (cerror) {
            SMBWARNING_LOCK(np, "error %d closing fid %llx file %s\n", cerror, fid, np->n_name);
        }
    }

    if (!error && !bcmp(sb, smb_symmagic, SMB_SYMMAGICLEN)) {
        for (cp = &sb[SMB_SYMMAGICLEN]; cp < &sb[SMB_SYMMAGICLEN+SMB_SYMLENLEN-1]; cp++) {
            if (!isdigit(*cp))
                break;
            len *= 10;
            len += *cp - '0';
        }
        cp++; /* skip newline */
        
        if ((cp != &sb[SMB_SYMMAGICLEN+SMB_SYMLENLEN]) ||
            (len > (int)(np->n_size - SMB_SYMHDRLEN))) {
            SMBWARNING("bad symlink length\n");
            error = ENOENT; /* Not a faked up symbolic link */
        } else {
            MD5Init(&md5);
            MD5Update(&md5, &sb[SMB_SYMHDRLEN], len);
            MD5Final((u_char *)state, &md5);
            (void)snprintf(m5b, sizeof(m5b), "%08x%08x%08x%08x",
                           htobel(state[0]), htobel(state[1]), htobel(state[2]),
                           htobel(state[3]));
            if (bcmp(cp, m5b, SMB_SYMMD5LEN-1)) {
                SMBWARNING("bad symlink md5\n");
                error = ENOENT; /* Not a faked up symbolic link */
            } else {
                *symlen = len;
                error = 0;
            }
        }
    }
    else {
		error = ENOENT; /* Not a faked up symbolic link */
    }
    
exit:
    if (uio != NULL) {
        uio_free(uio);
    }
    
    if (sb != NULL) {
        SMB_FREE(sb, M_TEMP);
    }
    
    return error;
}

/*
 * Lock a node
 */
int 
smbnode_lock(struct smbnode *np, enum smbfslocktype locktype)
{
	if (locktype == SMBFS_SHARED_LOCK)
		lck_rw_lock_shared(&np->n_rwlock);
	else
		lck_rw_lock_exclusive(&np->n_rwlock);

	np->n_lockState = locktype;
	
#if 1	
	/* For Debugging... */
	if (locktype != SMBFS_SHARED_LOCK) {
		np->n_activation = (void *) current_thread();
	}
#endif
	return (0);
}


/*
 * Lock a pair of smbnodes
 *
 * If the two nodes are not the same then lock in the order they came in. The calling routine
 * should always put them in parent/child order.
 */
int 
smbnode_lockpair(struct smbnode *np1, struct smbnode *np2, enum smbfslocktype locktype)
{
	int error;

	/*
	 * If smbnodes match then just lock one.
	 */
	if (np1 == np2) {
		return smbnode_lock(np1, locktype);
	}
	if ((error = smbnode_lock(np1, locktype)))
		return (error);
	if ((error = smbnode_lock(np2, locktype))) {
		smbnode_unlock(np1);
		return (error);
	}
	return (0);
}

/*
 * Unlock a cnode
 */
void 
smbnode_unlock(struct smbnode *np)
{
	/* The old code called lck_rw_done which is a non supported kpi */
	if (np->n_lockState == SMBFS_SHARED_LOCK) {
		/* 
		 * Should we keep a counter and set n_lockState to zero when the 
		 * counter goes to zero? We would need to lock the counter in that
		 * case.
		 */
		lck_rw_unlock_shared(&np->n_rwlock);
	} else {
		/* Note: SMBFS_RECLAIM_LOCK is really SMBFS_EXCLUSIVE_LOCK */ 
		np->n_lockState = 0;
		lck_rw_unlock_exclusive(&np->n_rwlock);
	}
}

/*
 * Unlock a pair of cnodes.
 */
void 
smbnode_unlockpair(struct smbnode *np1, struct smbnode *np2)
{
	smbnode_unlock(np1);
	if (np2 != np1)
		smbnode_unlock(np2);
}

static int
tolower(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z')
        ch = 'a' + (ch - 'A');
	
    return ch;
}

/*
 * SMB 2/3 - if the server supports File IDs, return ino as hashval
 * If no File IDs, create hashval from the name.  Currently we use strncasecmp 
 * to find a match, since it uses tolower, we should do the same when creating 
 * our hashval from the name.
 */
uint64_t
smbfs_hash(struct smb_share *share, uint64_t ino,
            const char *name, size_t nmlen)
{
	uint64_t v;
	
    /* if no share, just want hash from name */
    
    if ((share) && (SSTOVC(share)->vc_misc_flags & SMBV_HAS_FILEIDS)) {
        /* Server supports File IDs, use the inode number as hash value */
        if (ino == 0) {
            /* This should not happen */
            SMBERROR("node id of 0 for %s\n", name);
        }
        
        v = ino;
    }
    else {
        /* Server does not support File IDs, hash the name instead */
        for (v = FNV1_32_INIT; nmlen; name++, nmlen--) {
            v *= FNV_32_PRIME;
            v ^= (uint64_t)tolower((unsigned char)*name);
        }
    }
    
	return v;
}

void
smb_vhashrem(struct smbnode *np)
{
	smbfs_hash_lock(np->n_mount);
	if (np->n_hash.le_prev) {
		LIST_REMOVE(np, n_hash);
		np->n_hash.le_prev = NULL;
	}
	smbfs_hash_unlock(np->n_mount);
	return;
}

void 
smb_vhashadd(struct smbnode *np, uint64_t hashval)
{
	struct smbnode_hashhead	*nhpp;
	
	smbfs_hash_lock(np->n_mount);
	nhpp = SMBFS_NOHASH(np->n_mount, hashval);
	LIST_INSERT_HEAD(nhpp, np, n_hash);
	smbfs_hash_unlock(np->n_mount);
	return;
	
}

/* Returns 0 if the names match, non zero if they do not match */
static int
smbfs_check_name(struct smb_share *share,
                 const char *name1,
                 const char *name2,
                 size_t name_len)
{
    int ret_val = 0;
    
    if (SSTOVC(share)->vc_misc_flags & SMBV_OSX_SERVER) {
        /* Its OS X Server so we know for sure */
        if (SSTOVC(share)->vc_volume_caps & kAAPL_CASE_SENSITIVE) {
            /* Case Sensitive */
            ret_val = bcmp(name1, name2, name_len);
            return (ret_val);
        }
    }
    
    /* Not case sensitive */
    ret_val = strncasecmp(name1, name2, name_len);

    return (ret_val);
}

static vnode_t
smb_hashget(struct smbmount *smp, struct smbnode *dnp, uint64_t hashval,
			const char *name, size_t nmlen, size_t maxfilenamelen, 
			uint32_t node_flag, const char *sname)
{
	vnode_t	vp;
	struct smbnode_hashhead	*nhpp;
	struct smbnode *np;
	uint32_t vid;
	size_t snmlen = (sname) ? strnlen(sname, maxfilenamelen+1) : 0;
    struct smb_vc *vcp = NULL;
    
    if (smp->sm_share == NULL) {
        SMBERROR("smp->sm_share is NULL? \n");
        return (NULL);
    }
    
    vcp = SSTOVC(smp->sm_share);
    
loop:
	smbfs_hash_lock(smp);
	nhpp = SMBFS_NOHASH(smp, hashval);
	LIST_FOREACH(np, nhpp, n_hash) {
		/* 
		 * If we are only looking for a stream node then skip any other nodes. 
		 * If we are look for a directory or data node then skip any stream nodes.
		 */
		if ((np->n_flag & N_ISSTREAM) != node_flag)
			continue;
        
        if (vcp->vc_misc_flags & SMBV_HAS_FILEIDS) {
            /* 
             * Server supports File IDs - ID uniquely identifies the item
             */
            if (np->n_ino != hashval) {
                continue;
            }
        }
        else {
            /*
             * Server does not support File IDs
             * We currently assume the remote file system is case insensitive, since
             * we have no way of telling using the protocol. Someday I would like to
             * detect and if the server is case sensitive. If the server is case
             * sensitive then we should use bcmp, if case insensitive use strncasecmp.
             * NOTE: The strncasecmp routine really only does a tolower, not what we
             * really want but the best we can do at this time.
             */
            
            lck_rw_lock_shared(&np->n_parent_rwlock);
            lck_rw_lock_shared(&np->n_name_rwlock);
            if ((np->n_parent != dnp) || (np->n_nmlen != nmlen) ||
                (smbfs_check_name(smp->sm_share, name, np->n_name, nmlen) != 0)) {
                lck_rw_unlock_shared(&np->n_name_rwlock);
                lck_rw_unlock_shared(&np->n_parent_rwlock);
                continue;
            }
            lck_rw_unlock_shared(&np->n_name_rwlock);
            lck_rw_unlock_shared(&np->n_parent_rwlock);
        }
        
        if ((np->n_flag & NDELETEONCLOSE) ||
            (np->n_flag & NMARKEDFORDLETE)) {
            /* Skip nodes that are not in the name space anymore */
            continue;
        }

		/* If this is a stream make sure its the correct stream */
		if (np->n_flag & N_ISSTREAM) {
			DBG_ASSERT(sname);	/* Better be looking for a stream at this point */

            lck_rw_lock_shared(&np->n_name_rwlock);
            if ((np->n_snmlen != snmlen) ||
				(bcmp(sname, np->n_sname, snmlen) != 0)) {
				SMBERROR("We only support one stream and we found found %s looking for %s\n", 
						 np->n_sname, sname);
                lck_rw_unlock_shared(&np->n_name_rwlock);
				continue;
			}
            lck_rw_unlock_shared(&np->n_name_rwlock);
		}
        
		if (ISSET(np->n_flag, NALLOC)) {
			SET(np->n_flag, NWALLOC);
			(void)msleep((caddr_t)np, smp->sm_hashlock, PINOD|PDROP, "smb_ngetalloc", 0);
			goto loop;
		}
        
		if (ISSET(np->n_flag, NTRANSIT)) {
			SET(np->n_flag, NWTRANSIT);
			(void)msleep((caddr_t)np, smp->sm_hashlock, PINOD|PDROP, "smb_ngettransit", 0);
			goto loop;
		}
        
		vp = SMBTOV(np);
		vid = vnode_vid(vp);
        
		smbfs_hash_unlock(smp);
        
		if (vnode_getwithvid(vp, vid)) {
			return (NULL);
        }

		/* Always return the node locked */
        if ((smbnode_lock(np, SMBFS_EXCLUSIVE_LOCK)) != 0) {
            vnode_put(vp);
            return (NULL);
        }

		np->n_lastvop = smb_hashget;
		return (vp);
	}
    
	smbfs_hash_unlock(smp);
	return (NULL);
}

/*
 * We need to test to see if the vtype changed on the node. We currently only support
 * three types of vnodes (VDIR, VLNK, and VREG). If the network transacition came
 * from Unix extensions, Darwin or a create then we can just test to make sure the vtype
 * is the same. Otherwise we cannot tell the difference between a symbolic link and
 * a regular file at this point. So we just make sure it didn't change from a file
 * to a directory or vise versa. Also make sure it didn't change from a reparse point
 * to a non reparse point or vise versa.
 */
static int 
node_vtype_changed(vnode_t vp, enum vtype node_vtype, struct smbfattr *fap)
{
	int rt_value = FALSE;	/* Always default to it no changing */
	
	/* Root node can never change, bad things will happen */
	if (vnode_isvroot(vp))
		return FALSE;

	if (vnode_isnamedstream(vp))	/* Streams have no type so ignore them */
		return FALSE;
	
	
	/* 
	 * The vtype is valid, use it to make the decision, Unix extensions, Darwin
	 * or a create.
	 */
	if (fap->fa_valid_mask & FA_VTYPE_VALID) {
		if ((VTOSMB(vp)->n_flag & NWINDOWSYMLNK) && (fap->fa_vtype == VREG)) {
			/* 
			 * This is a windows fake symlink, so the node type will come is as
			 * a regular file. Never let it change unless the node type comes
			 * in as something other than a regular file.
			 */
			rt_value = FALSE;
		} else {
			rt_value = (fap->fa_vtype != node_vtype);
		}
		goto done;
	}
	
	/* Once a directory, always a directory*/
	if (((node_vtype == VDIR) && !(VTOSMB(vp)->n_dosattr & SMB_EFA_DIRECTORY)) ||
		((node_vtype != VDIR) && (VTOSMB(vp)->n_dosattr & SMB_EFA_DIRECTORY))) {
		rt_value = TRUE;
		goto done;
	}
	
	/* Once a reparse point, always a reprase point */
	if ((VTOSMB(vp)->n_dosattr &  SMB_EFA_REPARSE_POINT) != (fap->fa_attr & SMB_EFA_REPARSE_POINT)) {
		rt_value = TRUE;
		goto done;
	}
done:
	if (rt_value) {
		SMBWARNING_LOCK(VTOSMB(vp), "%s had node type and attr of %d 0x%x now its %d 0x%x\n",
                        VTOSMB(vp)->n_name, node_vtype, VTOSMB(vp)->n_dosattr,
                        fap->fa_vtype, fap->fa_attr);
	}
	return rt_value;
}

/* 
 * smbfs_nget
 *
 * When calling this routine remember if you get a vpp back and no error then
 * the smbnode is locked and you will need to unlock it.
 *
 * The calling routine must hold a reference on the share
 *
 */
int 
smbfs_nget(struct smb_share *share, struct mount *mp,
           vnode_t dvp, const char *name, size_t nmlen,
           struct smbfattr *fap, vnode_t *vpp,
           uint32_t cnflags, uint32_t flags,
           vfs_context_t context)
{
	struct smbmount *smp = VFSTOSMBFS(mp);
	struct smbnode *np, *dnp;
	vnode_t vp;
	int error = 0;
	uint64_t hashval;
	struct vnode_fsparam vfsp;
	int locked = 0;
	struct componentname cnp;
	trigger_info_t *ti;
	
    /*
     * Be careful as 
     * (1) dvp can be NULL
     * (2) name can be NULL
     * (3) fap can be NULL
     */
    
	*vpp = NULL;
    
	if (vfs_isforce(mp)) {
		return ENXIO;
    }
    
	if (!(flags & SMBFS_NGET_LOOKUP_ONLY)) {
        /* dvp is only required if we are possibly creating the vnode */
        if (smp->sm_rvp != NULL && dvp == NULL) {
            return EINVAL;
        }
    }
    
	if (nmlen == 2 && bcmp(name, "..", 2) == 0) {
		SMBDEBUG("do not call me with dotdot!\n");
		return EINVAL;
	} else if (nmlen == 1 && name[0] == '.') {
		SMBDEBUG("do not call me with dot!\n");
		return (EINVAL);
	}
    
	dnp = dvp ? VTOSMB(dvp) : NULL;
	if (dnp == NULL && dvp != NULL) {
		SMBDEBUG("dead parent vnode\n");
		return (EINVAL);
	}

	/* 
	 * If we are going to add it to the name cache, then make sure its the name 
	 * on the server that gets used 
	 */
	bzero(&cnp, sizeof(cnp));
	cnp.cn_nameptr = (char *)name;
	cnp.cn_namelen = (int)nmlen;
	cnp.cn_flags = cnflags;
	
	SMB_MALLOC(np, struct smbnode *, sizeof *np, M_SMBNODE, M_WAITOK | M_ZERO);
    
    hashval = smbfs_hash(share, (fap ? fap->fa_ino: 0), name, nmlen);

	if ((*vpp = smb_hashget(smp, dnp, hashval, name, nmlen,
							share->ss_maxfilenamelen, 0, NULL)) != NULL) {
        /* Found a pre existing vnode */
		DBG_ASSERT(!vnode_isnamedstream(*vpp));
        
        /* 
         * Must be v_get and we have a blank fap except for the fa_ino so dont
         * try to update the meta data cache for this vnode
         */
        if (flags & SMBFS_NGET_NO_CACHE_UPDATE) {
            /* not going to create a vnode so dont need np */
			SMB_FREE(np, M_SMBNODE);
			return (0);
        }
        
		if (fap && node_vtype_changed(*vpp, vnode_vtype(*vpp), fap)) {
			/* 
			 * The node we found has the wrong vtype. We need to remove this one
			 * and create the new entry. Purge the old node from the name cache, 
			 * remove it from our hash table, and clear its cache timer. 
			 */
			cache_purge(*vpp);
			smb_vhashrem(VTOSMB(*vpp));
			VTOSMB(*vpp)->attribute_cache_timer = 0;
			VTOSMB(*vpp)->n_symlink_cache_timer = 0;
			smbnode_unlock(VTOSMB(*vpp));	/* Release the smbnode lock */
			vnode_put(*vpp);
			/* Now fall through and create the node with the correct vtype */
			*vpp = NULL;
		}
        else {
            /* not going to create a vnode so dont need np */
			SMB_FREE(np, M_SMBNODE);

			/* update the attr_cache info, this is never a stream node */
			if (fap) {
				smbfs_attr_cacheenter(share, *vpp, fap, FALSE, context);
            }

			if (dvp && (cnp.cn_flags & MAKEENTRY)) {
				cache_enter(dvp, *vpp, &cnp);
            }

			return (0);
		}
	}

	/*
	 * If SMBFS_NGET_LOOKUP_ONLY set, then it is an explicit lookup
	 * for an existing vnode. Return if the vnode does not already exist.
	 */
	if (flags & SMBFS_NGET_LOOKUP_ONLY) {
		SMB_FREE(np, M_SMBNODE);
		return (ENOENT);
	}

    if (fap == NULL) {
        /* This should never happen */
        SMBERROR("fap is NULL! \n");
		SMB_FREE(np, M_SMBNODE);
		return (ENOENT);
    }
	
    lck_rw_init(&np->n_rwlock, smbfs_rwlock_group, smbfs_lock_attr);
	lck_rw_init(&np->n_name_rwlock, smbfs_rwlock_group, smbfs_lock_attr);
	lck_rw_init(&np->n_parent_rwlock, smbfs_rwlock_group, smbfs_lock_attr);
	(void) smbnode_lock(np, SMBFS_EXCLUSIVE_LOCK);
	/* if we error out, don't forget to unlock this */
	locked = 1;
	np->n_lastvop = smbfs_nget;

	/* 
	 * The node_vtype_changed routine looks at the attributes field to 
	 * detemine if the node has changed from being a reparse point. So before 
	 * entering the smbfs_attr_cacheenter we need to make sure that the attributes
	 * field has been set when the node is created.
	 * 
	 * We only set the ReparseTag here, once a tag is set its always set. We 
	 * use node_vtype_changed to test if a reparse point has been removed. 
	 */
	np->n_reparse_tag = fap->fa_reparse_tag;
	np->n_dosattr = fap->fa_attr;
	np->n_vnode = NULL;	/* redundant, but emphatic! */
	np->n_mount = smp;
	np->n_size = fap->fa_size;
	np->n_data_alloc = fap->fa_data_alloc;
	np->n_ino = fap->fa_ino;
    
    lck_rw_lock_exclusive(&np->n_name_rwlock);
	np->n_name = smb_strndup(name, nmlen);
    lck_rw_unlock_exclusive(&np->n_name_rwlock);
    
	np->n_nmlen = nmlen;
	/* Default to what we can do and Windows support */
	np->n_flags_mask = EXT_REQUIRED_BY_MAC;
    
    /*
     * n_uid and n_gid are set to KAUTH_UID_NONE/KAUTH_GID_NONE as the
     * default.
     *
     * If ACLs are retrieved for this node, then we will replace n_uid/n_gid
     * with a uid/gid that was mapped from the SID.
     *
     * When asked for the uid/gid, if they are default values, we return
     * uid/gid of the mounting user. If they are not set to default values,
     * then ACLs must have been retrieved and the uid/gid set, so we return
     * what ever value is set in n_uid/n_gid.
     */
	np->n_uid = KAUTH_UID_NONE;
	np->n_gid = KAUTH_GID_NONE;
    
    /*
     * n_nfs_uid/n_nfs_gid are the uid/gid from ACLs and from the NFS ACE.
     * We dont really do much with it because OS X <-> Windows, we cant really
     * trust its value. OS X <-> OS X we could trust its value.
     */
	np->n_nfs_uid = KAUTH_UID_NONE;
	np->n_nfs_gid = KAUTH_GID_NONE;
	SET(np->n_flag, NALLOC);
	smb_vhashadd(np, hashval);
	if (dvp) {
        lck_rw_lock_exclusive(&np->n_parent_rwlock);
		np->n_parent = dnp;
        lck_rw_unlock_exclusive(&np->n_parent_rwlock);
        
		if (!vnode_isvroot(dvp)) {
			/* Make sure we can get the vnode, we could have an unmount about to happen */
			if (vnode_get(dvp) == 0) {
				if (vnode_ref(dvp) == 0) {
                    /* If we can get a refcnt then mark the child */
					np->n_flag |= NREFPARENT;
                    vnode_put(dvp);
                    
                    /* Increment parent node's child refcnt */
                    OSIncrementAtomic(&dnp->n_child_refcnt);
                } else {
                    vnode_put(dvp);
                    error = EINVAL;
                    goto errout;
                }
			} else {
                error = EINVAL;
                goto errout;
            }
		}
	}

	vfsp.vnfs_mp = mp;
	vfsp.vnfs_vtype = fap->fa_vtype;
	vfsp.vnfs_str = "smbfs";
	vfsp.vnfs_dvp = dvp;
	vfsp.vnfs_fsnode = np;
	/* This will make sure we always have  a vp->v_name */
	vfsp.vnfs_cnp = &cnp;
	vfsp.vnfs_vops = smbfs_vnodeop_p;
	vfsp.vnfs_rdev = 0;	/* no VBLK or VCHR support */
	vfsp.vnfs_flags = (dvp && (cnp.cn_flags & MAKEENTRY)) ? 0 : VNFS_NOCACHE;
	vfsp.vnfs_markroot = (np->n_ino == smp->sm_root_ino);
	vfsp.vnfs_marksystem = 0;
	
	/*
	 * We are now safe to do lookups with the node. We need to be careful with 
	 * the n_vnode field and we should always check to make sure its not null 
	 * before access that field. The current code always makes that check.
	 * 
	 * So if this is the root vnode then we need to make sure we can access it 
	 * across network without any errors. We keep a reference on the root vnode 
	 * so this only happens once at mount time.
	 *
	 * If this is a regular file then we need to see if its one of our special 
	 * Windows symlink files.
	 */
	if ((vfsp.vnfs_vtype == VDIR) && (dvp == NULL) && (smp->sm_rvp == NULL) && 
		(np->n_ino == smp->sm_root_ino)) {
        /* Lookup the root vnode */
		error = smbfs_lookup(share, np, NULL, NULL, fap, context);
		if (error) {
			goto errout;
        }
		
        /* Update the root vnode hash value */
        smb_vhashrem(np);
        
        if (!(SSTOVC(share)->vc_misc_flags & SMBV_HAS_FILEIDS)) {
            /* 
             * Server does not support File IDs, so set root vnode File ID to 
             * be SMBFS_ROOT_INO */
            fap->fa_ino = SMBFS_ROOT_INO;
        }

        hashval = smbfs_hash(share, fap->fa_ino, name, nmlen);
        
        /* Update the root vnode File ID */
        smp->sm_root_ino = np->n_ino = fap->fa_ino;

        smb_vhashadd(np, hashval);
	} else if ((vfsp.vnfs_vtype == VREG) && (np->n_size == SMB_SYMLEN)) {
		int symlen = 0;
		DBG_ASSERT(dvp);
		if (smb_check_for_windows_symlink(share, np, &symlen, context) == 0) {
			vfsp.vnfs_vtype = VLNK;
			fap->fa_valid_mask |= FA_VTYPE_VALID;
			fap->fa_vtype = VLNK;
			np->n_size = symlen;
			np->n_flag |= NWINDOWSYMLNK;			
		}
	}
	vfsp.vnfs_filesize = np->n_size;

	if ((np->n_dosattr & SMB_EFA_REPARSE_POINT) && 
		(np->n_reparse_tag != IO_REPARSE_TAG_DFS) && 
		(np->n_reparse_tag != IO_REPARSE_TAG_SYMLINK))  {
        SMBWARNING_LOCK(np, "%s - unknown reparse point tag 0x%x\n", np->n_name, np->n_reparse_tag);
	}
	
	if ((np->n_dosattr & SMB_EFA_REPARSE_POINT) && 
		(np->n_reparse_tag == IO_REPARSE_TAG_DFS)) {
		struct vnode_trigger_param vtp;

		bcopy(&vfsp, &vtp.vnt_params, sizeof(vfsp));
		ti = trigger_new(&vtp, smbfs_trigger_get_mount_args, smbfs_trigger_rel_mount_args);
		error = vnode_create(VNCREATE_TRIGGER, (uint32_t)VNCREATE_TRIGGER_SIZE, &vtp, &vp);
		if (error)
			trigger_free(ti);
	} else {
		error = vnode_create(VNCREATE_FLAVOR, (uint32_t)VCREATESIZE, &vfsp, &vp);
	}

	if (error)
		goto errout;
	vnode_settag(vp, VT_CIFS);
	np->n_vnode = vp;
	/*
	 * We now know what type of node we have so set the mode bit here. We never
	 * want to change this for the life of this node. If the type changes on
	 * the server then we will blow away this node and create a new one.
	 */
	switch (vnode_vtype(vp)) {
	    case VREG:
			np->n_mode |= S_IFREG;
			break;
	    case VLNK:
			np->n_mode |= S_IFLNK;
			break;
	    case VDIR:
			np->n_mode |= S_IFDIR;
			break;
	    default:
			SMBERROR("vnode_vtype %d\n", vnode_vtype(vp));
			np->n_mode |= S_IFREG;	/* Can't happen, but just to be safe */
	}
	
	/* Initialize the lock used for the open state, open deny list and resource size/timer */
	if (!vnode_isdir(vp)) {
		lck_mtx_init(&np->f_openStateLock, smbfs_mutex_group, smbfs_lock_attr);
		lck_mtx_init(&np->f_clusterWriteLock, smbfs_mutex_group, smbfs_lock_attr);
		lck_mtx_init(&np->rfrkMetaLock, smbfs_mutex_group, smbfs_lock_attr);
		lck_mtx_init(&np->f_openDenyListLock, smbfs_mutex_group, smbfs_lock_attr);
	}

	lck_mtx_init(&np->f_ACLCacheLock, smbfs_mutex_group, smbfs_lock_attr);
	/* update the attr_cache info, this is never a stream node */
	smbfs_attr_cacheenter(share, vp, fap, FALSE, context);

	*vpp = vp;
	CLR(np->n_flag, NALLOC);
        if (ISSET(np->n_flag, NWALLOC))
                wakeup(np);
	return 0;
    
errout:
	if (np->n_flag & NREFPARENT) {
		if (vnode_get(dvp) == 0) {
			vnode_rele(dvp);
			vnode_put(dvp);			
		}
		np->n_flag &= ~NREFPARENT;
        
        /* Remove the child refcnt from the parent we just added above */
        OSDecrementAtomic(&dnp->n_child_refcnt);
	}
	
	smb_vhashrem(np);
	
	if (locked == 1)
		smbnode_unlock(np);	/* Release the smbnode lock */

	if (ISSET(np->n_flag, NWALLOC))
		wakeup(np);
		
    lck_rw_lock_exclusive(&np->n_name_rwlock);
    if (np->n_name != NULL) {
        SMB_FREE(np->n_name, M_SMBNODENAME);
        np->n_name = NULL; /* Catch anyone still refering to np->n_name */
    }
    lck_rw_unlock_exclusive(&np->n_name_rwlock);
    
	lck_rw_destroy(&np->n_rwlock, smbfs_rwlock_group);
	lck_rw_destroy(&np->n_name_rwlock, smbfs_rwlock_group);
	lck_rw_destroy(&np->n_parent_rwlock, smbfs_rwlock_group);
    
	SMB_FREE(np, M_SMBNODE);
    
	return error;
}

/* 
* smbfs_find_vgetstrm
 *
 * When calling this routine remember if you get a vpp back and no error then
 * the smbnode is locked and you will need to unlock it.
 */
vnode_t 
smbfs_find_vgetstrm(struct smbmount *smp, struct smbnode *np, const char *sname,
					size_t maxfilenamelen)
{
	uint64_t hashval;
    vnode_t ret_vnode = NULL;
    
    lck_rw_lock_shared(&np->n_name_rwlock);
    
    hashval = smbfs_hash(smp->sm_share, np->n_ino, np->n_name, np->n_nmlen);
	ret_vnode = smb_hashget(smp, np, hashval, np->n_name, np->n_nmlen, maxfilenamelen,
					   N_ISSTREAM, sname);
    
    lck_rw_unlock_shared(&np->n_name_rwlock);
    
	return(ret_vnode);
}

/* 
* smbfs_vgetstrm
 *
 * When calling this routine remember if you get a vpp back and no error then
 * the smbnode is locked and you will need to unlock it.
 *
 * The calling routine must hold a reference on the share
 *
 */
int 
smbfs_vgetstrm(struct smb_share *share, struct smbmount *smp, vnode_t vp, 
			   vnode_t *svpp, struct smbfattr *fap, const char *sname)
{
	struct smbnode *np, *snp;
	int error = 0;
	uint64_t hashval;
	struct vnode_fsparam vfsp;
	int locked = 0;
	struct componentname cnp;
	size_t maxfilenamelen = share->ss_maxfilenamelen;
    char *tmp_namep = NULL;
	
	/* Better have a root vnode at this point */
	DBG_ASSERT(smp->sm_rvp);
	/* Better have a parent vnode at this point */
	DBG_ASSERT(vp);
	/* Parent vnode better not be a directory */
	DBG_ASSERT((!vnode_isdir(vp)));
	/* Parent vnode better not be a stream */
	DBG_ASSERT(!vnode_isnamedstream(vp));
	np = VTOSMB(vp);
	*svpp = NULL;
	
	if (vfs_isforce(smp->sm_mp))
		return ENXIO;	
	/* Make sure the we have the correct name, always return the xattr name */
	bzero(&cnp, sizeof(cnp));
	cnp.cn_nameiop = LOOKUP;
	cnp.cn_flags = ISLASTCN;
	cnp.cn_pnlen = MAXPATHLEN;
	SMB_MALLOC (cnp.cn_pnbuf, caddr_t, MAXPATHLEN, M_TEMP, M_WAITOK);
	if (bcmp(sname, SFM_RESOURCEFORK_NAME, sizeof(SFM_RESOURCEFORK_NAME)) == 0) {
		cnp.cn_nameptr = cnp.cn_pnbuf;
        lck_rw_lock_shared(&np->n_name_rwlock);
		cnp.cn_namelen = snprintf(cnp.cn_nameptr, MAXPATHLEN, "%s%s", np->n_name,
								  _PATH_RSRCFORKSPEC);
        lck_rw_unlock_shared(&np->n_name_rwlock);
	}
    else {
		cnp.cn_nameptr = cnp.cn_pnbuf;
        lck_rw_lock_shared(&np->n_name_rwlock);
		cnp.cn_namelen = snprintf(cnp.cn_nameptr, MAXPATHLEN, "%s%s%s", np->n_name,
								  _PATH_FORKSPECIFIER, sname);
        lck_rw_unlock_shared(&np->n_name_rwlock);
		SMBWARNING("Creating non resource fork named stream: %s\n", cnp.cn_nameptr);
	}

	SMB_MALLOC(snp, struct smbnode *, sizeof *snp, M_SMBNODE, M_WAITOK);

    lck_rw_lock_shared(&np->n_name_rwlock);
    hashval = smbfs_hash(share, fap->fa_ino, np->n_name, np->n_nmlen);
	if ((*svpp = smb_hashget(smp, np, hashval, np->n_name, np->n_nmlen,
							 maxfilenamelen, N_ISSTREAM, sname)) != NULL) {
        lck_rw_unlock_shared(&np->n_name_rwlock);
		SMB_FREE(snp, M_SMBNODE);
		/* 
		 * If this is the resource stream then the parents resource fork size 
		 * has already been update. The calling routine aleady updated it. 
		 * Remember that the parent is currently locked. smbfs_attr_cacheenter 
		 * can lock the parent if we tell it to update the parent, so never tell 
		 * it to update the parent in this routine. 
		 */
		smbfs_attr_cacheenter(share, *svpp, fap, FALSE, NULL);
		goto done;
	}
    lck_rw_unlock_shared(&np->n_name_rwlock);

	bzero(snp, sizeof(*snp));
	lck_rw_init(&snp->n_rwlock, smbfs_rwlock_group, smbfs_lock_attr);
	lck_rw_init(&snp->n_name_rwlock, smbfs_rwlock_group, smbfs_lock_attr);
	lck_rw_init(&snp->n_parent_rwlock, smbfs_rwlock_group, smbfs_lock_attr);
	(void) smbnode_lock(snp, SMBFS_EXCLUSIVE_LOCK);
	locked = 1;
	snp->n_lastvop = smbfs_vgetstrm;
	
	snp->n_mount = smp;
	snp->n_size =  fap->fa_size;
	snp->n_data_alloc = fap->fa_data_alloc;
	snp->n_ino = np->n_ino;
    
    lck_rw_lock_shared(&np->n_name_rwlock);
    tmp_namep = smb_strndup(np->n_name, np->n_nmlen);
    lck_rw_unlock_shared(&np->n_name_rwlock);
    
    lck_rw_lock_exclusive(&snp->n_name_rwlock);
	snp->n_name = tmp_namep;
    lck_rw_unlock_exclusive(&snp->n_name_rwlock);
    
	snp->n_nmlen = np->n_nmlen;
	snp->n_flags_mask = np->n_flags_mask;
	snp->n_uid = np->n_uid;
	snp->n_gid = np->n_gid;
	snp->n_nfs_uid = np->n_nfs_uid;
	snp->n_nfs_gid = np->n_nfs_uid;
    
    lck_rw_lock_exclusive(&snp->n_parent_rwlock);
	snp->n_parent = np;
    lck_rw_unlock_exclusive(&snp->n_parent_rwlock);
    
	/* Only a stream node can have a stream name */
	snp->n_snmlen = strnlen(sname, maxfilenamelen+1);
    lck_rw_lock_exclusive(&snp->n_name_rwlock);
	snp->n_sname = smb_strndup(sname, snp->n_snmlen);
    lck_rw_unlock_exclusive(&snp->n_name_rwlock);
	
	SET(snp->n_flag, N_ISSTREAM);
	/* Special case that I would like to remove some day */
	if (bcmp(sname, SFM_RESOURCEFORK_NAME, sizeof(SFM_RESOURCEFORK_NAME)) == 0)
		SET(snp->n_flag, N_ISRSRCFRK);
	SET(snp->n_flag, NALLOC);
	smb_vhashadd(snp, hashval);
    
#ifdef _NOT_YET_
    /* Note: Temporarily commenting this out, see <rdar://problem/10695860> */
    
    /* Make sure we can get the parent vnode, we could have an unmount about to happen */
    if (!vnode_isvroot(vp)) {
        if (vnode_get(vp) == 0) {
            if (vnode_ref(vp) == 0) {
                /* If we can get a refcnt then mark the child */
                snp->n_flag |= NREFPARENT;
                vnode_put(vp);
                
                /* Increment parent node's child refcnt */
                OSIncrementAtomic(&np->n_child_refcnt);
            } else {
                vnode_put(vp);
                error = EINVAL;
                goto errout;
            }
        } else {
            error = EINVAL;
            goto errout;
        }
    }
#endif    
	vfsp.vnfs_mp = smp->sm_mp;
	vfsp.vnfs_vtype = VREG;
	vfsp.vnfs_str = "smbfs";
	vfsp.vnfs_dvp = NULL;
	vfsp.vnfs_fsnode = snp;
	/* This will make sure we always have  a vp->v_name */
	vfsp.vnfs_cnp = &cnp;
	vfsp.vnfs_vops = smbfs_vnodeop_p;
	vfsp.vnfs_rdev = 0;	/* no VBLK or VCHR support */
	vfsp.vnfs_flags = VNFS_NOCACHE;
	vfsp.vnfs_markroot = 0;
	vfsp.vnfs_marksystem = 0;
	vfsp.vnfs_filesize = fap->fa_size;
	
	error = vnode_create(VNCREATE_FLAVOR, (uint32_t)VCREATESIZE, &vfsp, svpp);
	if (error)
		goto errout;
	vnode_settag(*svpp, VT_CIFS);
	snp->n_vnode = *svpp;
	/*
	 * We now know what type of node we have so set the mode bit here. We never
	 * what to change this for the life of this node. If the type changes on
	 * the server then we will blow away this node and create a new one.
	 *
	 * Streams are aways regular files and have the parent node's access.
	 *
	 */
	snp->n_mode = S_IFREG | (np->n_mode & ACCESSPERMS);

	lck_mtx_init(&snp->f_openStateLock, smbfs_mutex_group, smbfs_lock_attr);
	lck_mtx_init(&snp->f_clusterWriteLock, smbfs_mutex_group, smbfs_lock_attr);
	lck_mtx_init(&snp->f_openDenyListLock, smbfs_mutex_group, smbfs_lock_attr);
	/* 
	 * If this is the resource stream then the parents resource fork size has 
	 * already been update. The calling routine aleady updated it. Remember that 
	 * the parent is currently locked. smbfs_attr_cacheenter can lock the parent 
	 * if we tell it to update the parent, so never tell it to update the parent 
	 * in this routine. 
	 */
	smbfs_attr_cacheenter(share, *svpp, fap, FALSE, NULL);
	
	CLR(snp->n_flag, NALLOC);
	if (ISSET(snp->n_flag, NWALLOC))
		wakeup(snp);
	goto done;
	
errout:
#ifdef _NOT_YET_
    /* Note: Temporarily commenting this out, see <rdar://problem/10695860> */
	if (snp->n_flag & NREFPARENT) {
		if (vnode_get(vp) == 0) {
			vnode_rele(vp);
			vnode_put(vp);			
		}
		snp->n_flag &= ~NREFPARENT;
        
        /* Remove the child refcnt from the parent we just added above */
        OSDecrementAtomic(&np->n_child_refcnt);
	}
#endif
    
	smb_vhashrem(snp);
	
	if (locked == 1)
		smbnode_unlock(snp);	/* Release the smbnode lock */
	
	if (ISSET(snp->n_flag, NWALLOC))
		wakeup(snp);
	
    lck_rw_lock_exclusive(&snp->n_name_rwlock);
    if (snp->n_name != NULL) {
        SMB_FREE(snp->n_name, M_SMBNODENAME);
        snp->n_name = NULL; /* Catch anyone still refering to np->n_name */
    }

    if (snp->n_sname != NULL) {
        SMB_FREE(snp->n_sname, M_SMBNODENAME);
        snp->n_sname = NULL; /* Catch anyone still refering to np->n_sname */
    }
    lck_rw_unlock_exclusive(&snp->n_name_rwlock);

    lck_rw_destroy(&snp->n_rwlock, smbfs_rwlock_group);
	lck_rw_destroy(&snp->n_name_rwlock, smbfs_rwlock_group);
	lck_rw_destroy(&snp->n_parent_rwlock, smbfs_rwlock_group);
    
	SMB_FREE(snp, M_SMBNODE);

done:	
	SMB_FREE(cnp.cn_pnbuf, M_TEMP);
	return error;
}

/* 
 * Update the nodes resource fork size if needed. 
 * NOTE: Remember the parent can lock the child while hold its lock, but the 
 * child cannot lock the parent unless the child is not holding its lock. So 
 * this routine is safe, because the parent is locking the child.
 *
 * The calling routine must hold a reference on the share
 *
 */
int 
smb_get_rsrcfrk_size(struct smb_share *share, vnode_t vp, vfs_context_t context)
{
	struct smbnode *np = VTOSMB(vp);
	uint64_t strmsize = 0;
	uint64_t strmsize_alloc = 0;
	time_t attrtimeo;
	struct timespec ts;
	int error = 0;
	time_t rfrk_cache_timer;
	struct timespec reqtime;
    uint32_t stream_flags = 0;
	int use_cached_data = 0;
	
    /* If we are in reconnect, use cached data if we have it */
    if (np->rfrk_cache_timer != 0) {
        use_cached_data = (share->ss_flags & SMBS_RECONNECTING);
    }

	nanouptime(&reqtime);

    /* Check to see if the cache has timed out */
    SMB_CACHE_TIME(ts, np, attrtimeo);
    
	lck_mtx_lock(&np->rfrkMetaLock);
	rfrk_cache_timer = ts.tv_sec - np->rfrk_cache_timer;
	lck_mtx_unlock(&np->rfrkMetaLock);
    
	if ((rfrk_cache_timer > attrtimeo) && !use_cached_data) {
        /* Cache has expired go get the resource fork size. */
		error = smbfs_smb_qstreaminfo(share, np, VREG,
                                      NULL, 0,
                                      SFM_RESOURCEFORK_NAME,
                                      NULL, NULL,
                                      &strmsize, &strmsize_alloc,
                                      &stream_flags, NULL,
                                      context);
        
        if ((error == ETIMEDOUT) && (np->rfrk_cache_timer != 0)) {
            /* Just return the cached data */
            error = 0;
            goto done;
        }
        
		/* 
		 * We got the resource stream size from the server, now update the resource
		 * stream if we have one. Search our hash table and see if we have a stream,
		 * if we find one then smbfs_find_vgetstrm will return it with a vnode_get
		 * and a smb node lock on it.
		 */
		if (error == 0) {
			struct smbmount *smp = VTOSMBFS(vp);
			vnode_t svpp = smbfs_find_vgetstrm(smp, np, SFM_RESOURCEFORK_NAME, 
											   share->ss_maxfilenamelen);

			if (svpp) {
				if (smbfs_update_size(VTOSMB(svpp), &reqtime, strmsize) == TRUE) {
					/* Remember the only attribute for a stream is its size */
					nanouptime(&ts);
					VTOSMB(svpp)->attribute_cache_timer = ts.tv_sec;			
				}
				smbnode_unlock(VTOSMB(svpp));
				vnode_put(svpp);
			}
		}
        else {
			/* 
			 * Remember that smbfs_smb_qstreaminfo will update the resource forks 
			 * cache and size if it finds  the resource fork. We are handling the 
			 * negative cache timer here. If we get an error then there is no 
			 * resource fork so update the cache.
			 */
			lck_mtx_lock(&np->rfrkMetaLock);
			np->rfrk_size = 0;
			np->rfrk_alloc_size = 0;
			nanouptime(&ts);
			np->rfrk_cache_timer = ts.tv_sec;
			lck_mtx_unlock(&np->rfrkMetaLock);
		}
	}

done:
	return(error);
}

/*
 * Anytime the stream is updated we need to update the parent's meta data. In 
 * the resource fork case this means updating the resource size and the resource 
 * size cache timer. For other streams it just means clearing the meta data cache
 * timer. We can update the parent's resource stream size and resource cache timer
 * here because we don't need the parent locked in this case. We use a different 
 * lock when updating the parents resource size and resource cache timer. Since we 
 * cannot lock the parent node here just return the parent vnode so the calling
 * process can handle clearing the meta data cache timer.
 *
 * NOTE:	smbfs_vnop_pageout calls this routine wihout the node locked. It is
 *			not setting the size so this should be safe. If anyone edits this  
 *			routine they need to keep in mind that it can be entered without a lock.
 */
vnode_t 
smb_update_rsrc_and_getparent(vnode_t vp, int setsize)
{
	struct smbnode *np = VTOSMB(vp);
	vnode_t parent_vp = vnode_getparent(vp);
	struct timespec ts;

	/* If this is a resource then update the parents resource size and cache timer */
	if ((parent_vp) && (np->n_flag & N_ISRSRCFRK)) {
		lck_mtx_lock(&VTOSMB(parent_vp)->rfrkMetaLock);
		
		/* They want us to update the size */
		if (setsize) {
			VTOSMB(parent_vp)->rfrk_size = np->n_size;
            /* assume alloc size same as new size */
			VTOSMB(parent_vp)->rfrk_alloc_size = np->n_size;
			nanouptime(&ts);
			VTOSMB(parent_vp)->rfrk_cache_timer = ts.tv_sec;					
		} else if (VTOSMB(parent_vp)->rfrk_size != np->n_size) {
			/* Something changed just reset the cache timer */
			VTOSMB(parent_vp)->rfrk_cache_timer = 0;
		}
		lck_mtx_unlock(&VTOSMB(parent_vp)->rfrkMetaLock);
	}
	return(parent_vp);	
}

static int 
smb_gid_match(struct smbmount *smp, u_int64_t node_gid)
{
    u_int32_t ii;
	
	if (node_gid == smp->ntwrk_gid)
		return TRUE;
	
	for (ii=0; ii < smp->ntwrk_cnt_gid; ii++)
		if (node_gid == smp->ntwrk_gids[ii])
			return TRUE;
	return FALSE;
}

/*
 * Check to see if the user has the request access privileges on the node. 
 * Someday we may have a call to check the access across the network, but for 
 * now all we can do is check the posix mode bits. 
 *
 * NOTE: rq_mode should be one of the S_IRWXO modes.
 */
int 
smb_check_posix_access(vfs_context_t context, struct smbnode * np, 
					   mode_t rq_mode)
{
	kauth_cred_t cred = vfs_context_ucred(context);
	uid_t	user = kauth_cred_getuid (cred);
	int		inGroup = 0;
	
	kauth_cred_ismember_gid(cred, np->n_gid, &inGroup);
	if (user == np->n_uid) {
		if (np->n_mode & (rq_mode << 6))
			return TRUE;
	} else if (inGroup) {
		if (np->n_mode & (rq_mode << 3))
			return TRUE;
	} else {
		if (np->n_mode & rq_mode)
			return TRUE;
	}
	return FALSE;
}

void smb_get_uid_gid_mode(struct smb_share *share, struct smbmount *smp,
                          struct smbfattr *fap, uint32_t flags,
                          uid_t *uid, gid_t *gid, mode_t *mode)
{
    uint16_t temp_mode = 0;
    
	if (fap->fa_unix) {
        /* Only SMB 1 supports Unix Extensions */
		if ((fap->fa_valid_mask & FA_UNIX_MODES_VALID) != FA_UNIX_MODES_VALID) {
			/*
			 * The call made to get this information did not contain the uid,
			 * gid or posix modes. So just keep using the ones we have, unless
			 * we have uninitialize values, then use the default values.
			 */
			if (*uid == KAUTH_UID_NONE) {
				*uid = smp->sm_args.uid;
				if (flags & SMBFS_GET_UGM_IS_DIR) {
					*mode |= smp->sm_args.dir_mode;
				} else {
					*mode |= smp->sm_args.file_mode;
				}
			}
            
			if (*gid == KAUTH_GID_NONE) {
				*gid = smp->sm_args.gid;
			} 			
		} else if (smp->sm_args.altflags & SMBFS_MNT_TIME_MACHINE) {
			/* Remove any existing modes. */
			*mode &= ~ACCESSPERMS;
            
			/* Just return what was passed into us */
			*uid = smp->sm_args.uid;
			*gid = smp->sm_args.gid;
			*mode |= (mode_t)(fap->fa_permissions & ACCESSPERMS);
		} else if (share->ss_attributes & FILE_PERSISTENT_ACLS) {
			/* Remove any existing modes. */
			*mode &= ~ACCESSPERMS;
            
			/* 
			 * The server supports the uid and gid and posix modes, so use the
			 * ones returned in the lookup call. If mapping then used the mounted
			 * users.
			 */
			if ((smp->sm_flags & MNT_MAPS_NETWORK_LOCAL_USER) && 
				(smp->ntwrk_uid == fap->fa_uid)) {
				*uid = smp->sm_args.uid;
				*gid = smp->sm_args.gid;
			}
            else {
				*uid = (uid_t)fap->fa_uid;
				*gid = (gid_t)fap->fa_gid;
			}
			*mode |= (mode_t)(fap->fa_permissions & ACCESSPERMS);
		} else if ((fap->fa_permissions & ACCESSPERMS) &&
				   (smp->sm_args.uid == (uid_t)smp->ntwrk_uid) &&
				   (smp->sm_args.gid == (gid_t)smp->ntwrk_gid)) {
			/* Remove any existing modes. */
			*mode &= ~ACCESSPERMS;
            
			/* 
			 * The server gave us POSIX modes and the local user matches the network 
			 * user, so assume they are in the same directory name space. 
			 */
			*uid = (uid_t)fap->fa_uid;
			*gid = (gid_t)fap->fa_gid;
			*mode |= (mode_t)(fap->fa_permissions & ACCESSPERMS);
		}
        else {
			int uid_match = (fap->fa_uid == smp->ntwrk_uid);
			int gid_match = smb_gid_match(smp, fap->fa_gid);
			
			/* Remove any existing modes. */
			*mode &= ~ACCESSPERMS;
            
			*uid = smp->sm_args.uid;
			*gid = smp->sm_args.gid;
            
			/* 
			 * We have no idea let the server handle any access issues. This 
			 * is safe because we only allow root and the user that mount the
			 * volume to have access to this mount point
			 */
			if ((fap->fa_permissions & ACCESSPERMS) == 0)
				fap->fa_permissions = ACCESSPERMS;
			if (!uid_match && !gid_match) {
				/* Use other perms */
				*mode |= (mode_t)(fap->fa_permissions & S_IRWXO);
                
				/* use other for group */
				*mode |= (mode_t)((fap->fa_permissions & S_IRWXO) << 3);
                
				/* use other for owner */
				*mode |= (mode_t)((fap->fa_permissions & S_IRWXO) << 6);
			} else if (!uid_match && gid_match) {
				/* Use group and other perms  */
				*mode |= (mode_t)(fap->fa_permissions & (S_IRWXG | S_IRWXO));
                
				/* use group for owner */
				*mode |= (mode_t)((fap->fa_permissions & S_IRWXG) <<  3);
			} else if (uid_match && !gid_match) {
				/* Use owner and other perms */
				*mode |= (mode_t)(fap->fa_permissions & (S_IRWXU | S_IRWXO));
                
				/* use other for group */
				*mode |= (mode_t)((fap->fa_permissions & S_IRWXO) << 3);
			} else {
				/* Use owner, group and other perms */
				*mode |= (mode_t)(fap->fa_permissions & ACCESSPERMS);
			}
		}		
	}
    else {
        /*
         * See comments in smbfs_nget about n_uid and n_gid and 
         * KAUTH_UID_NONE/KAUTH_GID_NONE default values.
         */
        if ((*uid == KAUTH_UID_NONE) || (*gid == KAUTH_GID_NONE)) {
            /*
             * Either ACLs are off or no ACL retrieved for this item.
             * Return the mounting user uid/gid
             */
            *uid = smp->sm_args.uid;
            *gid = smp->sm_args.gid;
        }
        else {
            /*
             * uid/gid must have been set by a previous Get ACL, so just return
             * their current value.
             */
        }
        
        /* Figure out the mode */
        if (fap->fa_valid_mask & FA_UNIX_MODES_VALID) {
            /* 
             * Server gave us Posix modes via AAPL ReadDirAttr extension
             */
            
            /* Remove any existing modes. */
            *mode &= ~ACCESSPERMS;
            
            temp_mode = fap->fa_permissions;
            *mode |= (temp_mode & ACCESSPERMS); /* only take access perms */
        }
        else {
            if (flags & SMBFS_GET_UGM_REMOVE_POSIX_MODES) {
                /* Remove any existing modes. */
                *mode &= ~ACCESSPERMS;
                /*
                 * The system just can't handle posix modes of zero. We now support
                 * maximal access, so just dummy up the posix modes so copies work
                 * when all you have is inherited ACLs.
                 */
                if (flags & SMBFS_GET_UGM_IS_DIR) {
                    *mode |= smp->sm_args.dir_mode;
                }
                else {
                    /* symlink or regular file */
                    *mode |= smp->sm_args.file_mode;
                }
            }
        }
	}
}

/*
 * Check to see if the immutable bit should be set on this node. 
 *
 * SMB_EFA_RDONLY ~ UF_IMMUTABLE
 *
 * We treat the SMB_EFA_RDONLY as the immutable flag. This allows
 * us to support the finder lock bit and makes us follow the 
 * MSDOS code model. See msdosfs project.
 *
 * NOTE: The ready-only flags does not exactly follow the lock/immutable bit.
 *
 * See Radar 5582956 for more details.
 *
 * When dealing with Window Servers the read-only bit for folder does not  
 * mean the samething as it does for files. Doing this translation was
 * confusing customers and really didn't work the way Mac users would expect.
 */
Boolean
node_isimmutable(struct smb_share *share, vnode_t vp, struct smbfattr *fap)
{
	Boolean unix_info2 = ((UNIX_CAPS(share) & UNIX_QFILEINFO_UNIX_INFO2_CAP)) ? TRUE : FALSE;
	Boolean darwin = (SSTOVC(share)->vc_flags & SMBV_DARWIN) ? TRUE : FALSE;
    uint32_t is_dir = 0;
    uint32_t is_read_only = 0;
    
    if (vp != NULL) {
        if (vnode_isdir(vp)) {
            is_dir = 1;
        }
        
        if (VTOSMB(vp)->n_dosattr & SMB_EFA_RDONLY) {
            is_read_only = 1;
        }
    }
    else {
        if (fap != NULL) {
            /* smbfs_vnop_readdirattr or smbfs_vnop_getattrlistbulk */
            if (fap->fa_vtype == VDIR) {
                is_dir = 1;
            }
            
            if (fap->fa_attr & SMB_EFA_RDONLY) {
                is_read_only = 1;
            }
        }
        else {
            /* this should be impossible */
            SMBERROR("vp and fap are NULL \n");
        }
    }
	
    if (SSTOVC(share)->vc_flags & SMBV_SMB2) {
        if ((UNIX_SERVER(SSTOVC(share)) || !is_dir) && is_read_only) {
            return TRUE;
        }
    }
	else {
        if ((unix_info2 || darwin || !is_dir) && is_read_only) {
            return TRUE;
        }
    }
	return FALSE;
}

/*
 * routines to maintain vnode attributes cache
 *
 * The calling routine must hold a reference on the share
 *
 */
void 
smbfs_attr_cacheenter(struct smb_share *share, vnode_t vp, struct smbfattr *fap, 
					  int UpdateResourceParent, vfs_context_t context)
{
	struct smbmount *smp = VTOSMBFS(vp);
	struct smbnode *np = VTOSMB(vp);
	enum vtype node_vtype;
	struct timespec ts;
	uint32_t monitorHint = 0;
    uint32_t flags = 0;
    
	node_vtype = vnode_vtype(vp);

	if ((node_vtype == VDIR) && np->d_needsUpdate) {
		monitorHint |= VNODE_EVENT_ATTRIB | VNODE_EVENT_WRITE;
		np->d_needsUpdate = FALSE;			
	}
	
	/* 
	 * The vtype of node has changed, so remove it from the name cache and our 
	 * hash table. We set the cache timer to zero this will cause cache lookup 
	 * routine to return ENOENT.
	 */
	if (node_vtype_changed(vp, node_vtype, fap)) {
		np->attribute_cache_timer = 0;
		np->n_symlink_cache_timer = 0;
		cache_purge(vp);
		smb_vhashrem(np);
		monitorHint |= VNODE_EVENT_RENAME | VNODE_EVENT_ATTRIB;
		goto vnode_notify_needed;
	}

	/* No need to update the cache after close, we just got updated */
	np->n_flag &= ~NATTRCHANGED;
	if (node_vtype == VREG) {
		if (smbfs_update_size(np, &fap->fa_reqtime, fap->fa_size) == FALSE) {
			/* We lost the race, assume we have the correct size */
			fap->fa_size = np->n_size;
			/* Force a lookup on close, make sure we have the correct size on close */
			np->n_flag |= NATTRCHANGED;
 		} else if (np->n_size != fap->fa_size) {
			/* We one the race and the size change, notify them about the change */
			monitorHint |= VNODE_EVENT_EXTEND | VNODE_EVENT_ATTRIB;
		}
	} else if (node_vtype == VDIR) {
		np->n_size = 16384; 	/* should be a better way ... */
		/* See if we need to clear the negative name cache */
		if ((np->n_flag & NNEGNCENTRIES) && 
			((share->ss_fstype == SMB_FS_FAT) || 
			 (timespeccmp(&fap->fa_mtime, &np->n_mtime, >)))) {
			np->n_flag &= ~NNEGNCENTRIES;
			cache_purge_negatives(vp);			
                
            VTOSMB(vp)->d_changecnt++;
		}
		/*
		 * Don't allow mtime to go backwards.
		 * Yes this has its flaws.  Better ideas are welcome!
		 */
		if (timespeccmp(&fap->fa_mtime, &np->n_mtime, <))
			fap->fa_mtime = np->n_mtime;
	} else if (node_vtype != VLNK) {
		return;
	}
	/* The server told us the allocation size return what they told us */
	np->n_data_alloc = fap->fa_data_alloc;
	
	if (fap->fa_unix) {
		np->n_flags_mask = fap->fa_flags_mask;
		np->n_nlinks = fap->fa_nlinks;
    }
    
    /* Calculate the uid, gid, and mode */
    if (vnode_isdir(np->n_vnode)) {
        flags |= SMBFS_GET_UGM_IS_DIR;
    }
    
    /* 
     * Unix mode can come from an ACL ACE (which sets NHAS_POSIXMODES)
     * or from SMB 2 when the FA_UNIX_MODES_VALID is set. Only dummy up 
     * fake modes if we dont have the unix modes already.
     */
    if (!(np->n_flag & NHAS_POSIXMODES) &&
        !(fap->fa_valid_mask & FA_UNIX_MODES_VALID)) {
        flags |= SMBFS_GET_UGM_REMOVE_POSIX_MODES;
    }
    
    smb_get_uid_gid_mode(share, smp,
                         fap, flags,
                         &np->n_uid, &np->n_gid, &np->n_mode);
    
    if (fap->fa_valid_mask & FA_UNIX_MODES_VALID) {
        np->n_flag |= NHAS_POSIXMODES;
    }
    
    if ((monitorHint & VNODE_EVENT_ATTRIB) == 0) {
		if (!(timespeccmp(&np->n_crtime, &fap->fa_crtime, ==) ||
			 !(timespeccmp(&np->n_mtime, &fap->fa_mtime, ==))))
			monitorHint |= VNODE_EVENT_ATTRIB;
	}
	
	/* 
	 * We always set the fstatus time if its valid
	 * Never reset the fstatus if the following are true:
	 * 1. The modify time on the item hasn't changed.
	 * 2. We have already discovered that this item has no streams.
	 * 3. The fap information didn't come from an open call.
	 *
	 * NOTE: This needs to be done before we update the modify time.
	 */
	if (fap->fa_valid_mask & FA_FSTATUS_VALID) {
		/* This is a valid field use it */
		np->n_fstatus = fap->fa_fstatus;
	} else if (timespeccmp(&np->n_chtime, &fap->fa_chtime, !=)) {
		/* 
		 * Something change clear the fstatus field sine we can't trust it 
		 * NOTE: The above check needs to be done before we update the change time.
		 */
		np->n_fstatus = 0;
	}

    /* Update max access if its valid */
	if (fap->fa_valid_mask & FA_MAX_ACCESS_VALID) {
        np->maxAccessRights = fap->fa_max_access;
        np->maxAccessRightChTime = fap->fa_chtime;
    }
    
    /*
	 * Not sure if this is still a problem. In the old days the finder did
	 * not like it when the create time of the root or directory was after
	 * the modify time. This can and will happen on FAT file systems. For
	 * now lets leave it alone and see what happens.
	 */
	np->n_crtime = fap->fa_crtime;
	np->n_chtime = fap->fa_chtime;
	np->n_atime = fap->fa_atime;
	np->n_mtime = fap->fa_mtime;
	/*
	 * This routine can be called by a Query Info, FindFirst or the NTCreateAndX
	 * routine. If the server doesn't support the UNIX extensions then the fa_unix
	 * field won't be set and fa_attr will contain the dos attributes. We map the
	 * hidden, read only and the archive bits to the hidden, immutable and 
	 * "not backed up" bits in the BSD flags.
	 *
	 * In the NTCreateAndX routine we check to see if the server supports the 
	 * UNIX extensions and we convert the fa_attr and fa_flags_mask to the correct
	 * values depending on the bits supported by the server. This allows us to
	 * always trust the values in the NTCreateAndX fap.
	 *
	 * Linux server do not support the UNIX Info2 BSD flags. This is a problem, 
	 * we still want to use the UNIX extensions, but we can't trust all the bits
	 * in the fa_attr field when they come from the Query Info or FindFirst 
	 * routine. So in this case ignore the hidden, read only and the archive bits
	 * in the fa_attr and just keep using the ones we have on the node. This means
	 * for Linux servers we only trust the bits that come from the NTCreateAndX or
	 * the bits we have set ourself. Remember we we lookup access with the NTCreateAndX
	 * so we have the latest info in that case.
	 */
	if (fap->fa_unix && ((fap->fa_flags_mask & EXT_REQUIRED_BY_MAC) != EXT_REQUIRED_BY_MAC)) {
		fap->fa_attr &= ~(SMB_EFA_RDONLY | SMB_EFA_HIDDEN | SMB_EFA_ARCHIVE);
		np->n_dosattr &= (SMB_EFA_RDONLY | SMB_EFA_HIDDEN | SMB_EFA_ARCHIVE);
		np->n_dosattr |= fap->fa_attr;
	} else {
		np->n_dosattr = fap->fa_attr;
	}

	nanouptime(&ts);
	np->attribute_cache_timer = ts.tv_sec;
	/*
	 * UpdateResourceParent says it is ok to update the parent if this is a 
	 * resource stream. So if this is a stream and its the resource stream then 
	 * update the parents resource fork size and cache timer. If we can't get the 
	 * parent then just get out, when the timer goes off the parent will just have 
	 * to make the wire call.
	 */
	if (UpdateResourceParent && (vnode_isnamedstream(vp)) && 
		(np->n_flag & N_ISRSRCFRK)) {
		vnode_t parent_vp = smb_update_rsrc_and_getparent(vp, (fap->fa_size) ? TRUE : FALSE);
		/* We no longer need the parent so release it. */
		if (parent_vp)  
			vnode_put(parent_vp);
	}
	
vnode_notify_needed:
	if ((monitorHint != 0) && (vnode_ismonitored(vp)) && context) {
		struct vnode_attr vattr;
		
		vfs_get_notify_attributes(&vattr);
		smbfs_attr_cachelookup(share, vp, &vattr, context, TRUE);
		vnode_notify(vp, monitorHint, &vattr);
	}
}

/*
 * The calling routine must hold a reference on the share
 */
int 
smbfs_attr_cachelookup(struct smb_share *share, vnode_t vp, struct vnode_attr *va, 
					   vfs_context_t context, int useCacheDataOnly)
{
	struct smbnode *np = VTOSMB(vp);
	struct smbmount *smp = VTOSMBFS(vp);
	time_t attrtimeo;
	struct timespec ts;
	
	SMB_CACHE_TIME(ts, np, attrtimeo);

	if (useCacheDataOnly) {
		/* Use the current cache data only */
	} else if (np->n_flag & NMARKEDFORDLETE) {
		/*
		 * The file is marked for deletion on close. We can no longer 
		 * gain access using the path. All access must be done using
		 * the fid. So just pretend that the cache will never expire 
		 * for this item. 
		 *
		 * NOTE: Since it is marked for deletion no one else can access
		 *       it, so the cache data should stay good through the life
		 *       of the open file.
		 */
	}
	else if ((ts.tv_sec - np->attribute_cache_timer) > attrtimeo)
		return (ENOENT);

	if (!va)
		return (0);
	
	VATTR_RETURN(va, va_rdev, 0);
	if ((UNIX_CAPS(share) & UNIX_QFILEINFO_UNIX_INFO2_CAP))
		VATTR_RETURN(va, va_nlink, np->n_nlinks);
	else
		VATTR_RETURN(va, va_nlink, 1);
	
	/* 
	 * Looks like we need to handle total size in the streams case. The VFS layer 
	 * always fill this in with the data fork size. Still not sure of this, but 
	 * for now lets go ahead and handle if ask.
	 */
	if ((share->ss_attributes & FILE_NAMED_STREAMS) && 
		(VATTR_IS_ACTIVE(va, va_total_size))) {
		if (vnode_isdir(vp)) {
			VATTR_RETURN(va, va_total_size, np->n_size);
			lck_mtx_lock(&smp->sm_statfslock);
			if (smp->sm_statfsbuf.f_bsize)	/* Just to be safe */
				VATTR_RETURN(va, va_total_alloc, roundup(va->va_total_size, 
														 smp->sm_statfsbuf.f_bsize));
			lck_mtx_unlock(&smp->sm_statfslock);
		}
		else if (!vnode_isnamedstream(vp)) {
			if (!useCacheDataOnly) {
				(void)smb_get_rsrcfrk_size(share, vp, context);
			}
			lck_mtx_lock(&np->rfrkMetaLock);
			VATTR_RETURN(va, va_total_size, np->n_size + np->rfrk_size);
			lck_mtx_unlock(&np->rfrkMetaLock);
			lck_mtx_lock(&smp->sm_statfslock);
			if (smp->sm_statfsbuf.f_bsize)	/* Just to be safe */
				VATTR_RETURN(va, va_total_alloc, roundup(va->va_total_size, 
														 smp->sm_statfsbuf.f_bsize));
			lck_mtx_unlock(&smp->sm_statfslock);
		}
	}
	
	VATTR_RETURN(va, va_data_size, np->n_size);
	VATTR_RETURN(va, va_data_alloc, np->n_data_alloc);
	VATTR_RETURN(va, va_iosize, smp->sm_statfsbuf.f_bsize);
	
	if (VATTR_IS_ACTIVE(va, va_mode))
		VATTR_RETURN(va, va_mode, np->n_mode);
		
	if (VATTR_IS_ACTIVE(va, va_uid) || VATTR_IS_ACTIVE(va, va_gid)) {
		/* 
		 * The volume was mounted as guest, so we already set the mount point to 
		 * ignore ownership. Now always return an owner of 99 and group of 99. 
		 */
		if (SMBV_HAS_GUEST_ACCESS(SSTOVC(share))) {
			VATTR_RETURN(va, va_uid, UNKNOWNUID);
			VATTR_RETURN(va, va_gid, UNKNOWNGID);
		} else {
			/* 
			 * For servers that support the UNIX extensions we know the uid/gid. 
			 * For server that don't support ACLs then the node uid/gid will be 
			 * set to the mounted user's  uid/gid. For all other servers we need 
			 * to get the ACL and translate the SID to a uid or gid. The uid/gid 
			 * really is for display purpose only and means nothing to us. We will 
			 * set the nodes ids if we get a request for the ACL, but otherwise
			 * we leave them unset for performance reasons.
			 */
			if (np->n_uid == KAUTH_UID_NONE)
				VATTR_RETURN(va, va_uid, smp->sm_args.uid);				
			else 
				VATTR_RETURN(va, va_uid, np->n_uid);
			if (np->n_gid == KAUTH_GID_NONE)
				VATTR_RETURN(va, va_gid, smp->sm_args.gid);			
			else
				VATTR_RETURN(va, va_gid, np->n_gid);			
		}
	}
	if (VATTR_IS_ACTIVE(va, va_flags)) {
		va->va_flags = 0;
		/*
		 * Remember that SMB_EFA_ARCHIVE means the items needs to be 
		 * archive and SF_ARCHIVED means the item has been archive.
		 *
		 * NOTE: Windows does not set ATTR_ARCHIVE bit for directories.
		 */
		if (!vnode_isdir(vp) && !(np->n_dosattr & SMB_EFA_ARCHIVE))
			va->va_flags |= SF_ARCHIVED;
		/* The server has it marked as read-only set the immutable bit. */
		if (node_isimmutable(share, vp, NULL)) {
			va->va_flags |= UF_IMMUTABLE;
		}
        
        /*
         * The server has it marked as hidden, set the new UF_HIDDEN bit. Never
         * mark the root volume as hidden.
         */
        if ((np->n_dosattr & SMB_EFA_HIDDEN) && !vnode_isvroot(vp)) {
            va->va_flags |= UF_HIDDEN;
        }
		VATTR_SET_SUPPORTED(va, va_flags);
	}
	
	/* va_acl are done in smbfs_getattr */
	
	VATTR_RETURN(va, va_create_time, np->n_crtime);
	VATTR_RETURN(va, va_modify_time, np->n_mtime);
	/* FAT only supports the date not the time! */
	VATTR_RETURN(va, va_access_time, np->n_atime);
	/* 
	 * FAT does not support change time, so just return the modify time. 
	 * Copied from the msdos code. SMB has no backup time so skip the
	 * va_backup_time.
	 */
	if (share->ss_fstype == SMB_FS_FAT)
		np->n_chtime.tv_sec = np->n_mtime.tv_sec;
	VATTR_RETURN(va, va_change_time, np->n_chtime);
	
	/*
	 * Exporting file IDs from HFS Plus:
	 *
	 * For "normal" files the c_fileid is the same value as the
	 * c_cnid.  But for hard link files, they are different - the
	 * c_cnid belongs to the active directory entry (ie the link)
	 * and the c_fileid is for the actual inode (ie the data file).
	 *
	 * The stat call (getattr) uses va_fileid and the Carbon APIs,
	 * which are hardlink-ignorant, will ask for va_linkid.
	 */
    lck_rw_lock_shared(&np->n_name_rwlock);
    VATTR_RETURN(va, va_fileid, smb2fs_smb_file_id_get(smp, np->n_ino,
                                                       np->n_name));
    VATTR_RETURN(va, va_linkid, smb2fs_smb_file_id_get(smp, np->n_ino,
                                                       np->n_name));
    lck_rw_unlock_shared(&np->n_name_rwlock);
    
    lck_rw_lock_shared(&np->n_parent_rwlock);
    if (np->n_parent != NULL) {
        lck_rw_lock_shared(&np->n_parent->n_name_rwlock);
        VATTR_RETURN(va, va_parentid, smb2fs_smb_file_id_get(smp,
                                                             np->n_parent->n_ino,
                                                             np->n_parent->n_name));
        lck_rw_unlock_shared(&np->n_parent->n_name_rwlock);
    }
    else {
        /*
         * This would require a lot more work so let the VFS layer handle it.  
         * VATTR_RETURN(va, va_parentid, np->n_parentid);
         */
    }
    lck_rw_unlock_shared(&np->n_parent_rwlock);

	VATTR_RETURN(va, va_fsid, vfs_statfs(vnode_mount(vp))->f_fsid.val[0]);
	VATTR_RETURN(va, va_filerev, 0);	
	VATTR_RETURN(va, va_gen, 0);
	
	/* 
	 * We currently have no way to know the va_encoding. The VFS layer fills it
	 * in with kTextEncodingMacUnicode = 0x7E. Lets leave it to the VFS layer
	 * to handle for now.
	 * VATTR_RETURN(va, va_encoding, 0x7E);
	 */
	
	/* 
	 * If this is the root, let VFS find out the mount name, which may be 
	 * different from the real name 
	 */
	if (VATTR_IS_ACTIVE(va, va_name) && !vnode_isvroot(vp)) {
        lck_rw_lock_shared(&np->n_name_rwlock);
		strlcpy ((char*) va->va_name, (char*)np->n_name, MAXPATHLEN);
        lck_rw_unlock_shared(&np->n_name_rwlock);
		VATTR_SET_SUPPORTED(va, va_name);
	}
	/* va_uuuid is done in smbfs_getattr */
	/* va_guuid is done in smbfs_getattr */
	/* We have no  way to get va_nchildren. Let VFS layer handle it. */
	return (0);
}

/*
 * FAT file systems don't exhibit POSIX behaviour with regard to 
 * updating the directory mtime when the directory's contents 
 * change.
 *
 * We force the issue here by updating our cached copy of the mtime
 * whenever we perform such an action ourselves, and then mark the
 * cache invalid.  Subsequently when the invalidated cache entry is
 * updated, we disallow an update that would move the mtime backwards.
 *
 * This preserves correct or near-correct behaviour with a
 * compliant server, and gives near-correct behaviour with
 * a non-compliant server in the most common case (we are the
 * only client changing the directory).  
 *
 * There are also complications if a server's time is ahead
 * of our own.  We must 'touch' a directory when it is first
 * created, to ensure that the timestamp starts out sane,
 * however it may have a timestamp well ahead of the 'touch'
 * point which will be returned and cached the first time the
 * directory's attributes are fetched.  Subsequently, the
 * directory's mtime will not appear to us to change at all
 * until our local time catches up to the server.
 *
 * Thus, any time a directory is 'touched', the saved timestamp
 * must advance at least far enough forwards to be visible to
 * the stat(2) interface.
 *
 * We only do this for FAT file system, all others should be handling
 * the modify time correctly.
 */
void
smbfs_attr_touchdir(struct smbnode *dnp, int fatShare)
{
	if (fatShare) {
		struct timespec ts, ta;
		
		/*
		 * Creep the saved time forwards far enough that
		 * layers above the kernel will notice.
		 */
		ta.tv_sec = 1;
		ta.tv_nsec = 0;
		timespecadd(&dnp->n_mtime, &ta);
		/*
		 * If the current time is later than the updated
		 * saved time, apply it instead.
		 */
		nanotime(&ts);	/* Need current date/time, so use nanotime */
		if (timespeccmp(&dnp->n_mtime, &ts, <))
			dnp->n_mtime = ts;		
	}
	/*
	 * Invalidate the cache, so that we go to the wire
	 * to check that the server doesn't have a better
	 * timestamp next time we care.
	 */
	dnp->attribute_cache_timer = 0;
}

int 
smbfsIsCacheable(vnode_t vp)
{
	/* Has to be a file, so dirs and symlinks are not cacheable */
	if (!vnode_isreg(vp)) {
		return FALSE;
	}
	if (vnode_isnocache(vp)) {
		return FALSE;
	} else {
		return TRUE;
	}
}

void
smbfs_setsize(vnode_t vp, off_t size)
{
	struct smbnode *np = VTOSMB(vp);

	/*
	 * n_size is used by smbfs_vnop_pageout so it must be
	 * changed before we call setsize
	 */
	np->n_size = size;
	ubc_setsize(vp, size);
	/*
	 * this lets us avoid a race with readdir which resulted in
	 * a stale n_size, which in the worst case yielded data corruption.
	 */
	nanouptime(&np->n_sizetime);
	/* Resetting the size, blow away statfs cache */
	VTOSMBFS(vp)->sm_statfstime = 0;
}

/*
 * If the file size hasn't change then really nothing to do here, get out but
 * let the calling routine know that they can update their cache timer. If we have
 * updated the size internally, while waiting on the response from the server,
 * then don't update the size and tell the calling routine not to update its
 * cache timers. Otherwise update our internal size and the ubc size. Also tell
 * the calling routine to update any cache timers.
 */
int 
smbfs_update_size(struct smbnode *np, struct timespec *reqtime, u_quad_t new_size)
{	
	if (np->n_size == new_size)
		return TRUE; /* Nothing to update here */
	
	/* Only update the size if we don't have a set eof pending */
	if (np->n_flag & NNEEDS_EOF_SET) {
        SMB_LOG_IO_LOCK(np, "%s: Waiting on pending seteof, old eof = %lld  new eof = %lld\n",
                        np->n_name, np->n_size, new_size);
		return FALSE;
	}
	
	if (np->waitOnClusterWrite) {
		SMB_LOG_IO_LOCK(np, "%s: Waiting on cluster write to complete, old eof = %lld  new eof = %lld\n",
                        np->n_name, np->n_size, new_size);
		return FALSE;
	}
	
	if (timespeccmp(reqtime, &np->n_sizetime, <=)) {
		SMB_LOG_IO_LOCK(np, "%s: We set the eof after this lookup, old eof = %lld  new eof = %lld\n",
                        np->n_name, np->n_size, new_size);
		return FALSE; /* we lost the race, tell the calling routine */
	}
	
	/* 
	 * The file size on the server is different from our copy. So can we trust
	 * any of our data? Should we push, invalidate the whole file?
	 *
	 * The old code would only invalidate the region that the file had grown. Now
	 * since we call ubc_setsize in smbfs_setsize that should handle any truncate
	 * issue. Not sure why you invalidate a region you don't even have in the cache?
	 */
	ubc_msync (np->n_vnode, 0, ubc_getsize(np->n_vnode), NULL, UBC_PUSHDIRTY | UBC_SYNC);
	
	SMBDEBUG_LOCK(np, "%s: smbfs_setsize, old eof = %lld  new eof = %lld time %ld:%ld  %ld:%ld\n",
                  np->n_name, np->n_size, new_size,
                  np->n_sizetime.tv_sec, np->n_sizetime.tv_nsec,
                  reqtime->tv_sec, reqtime->tv_nsec);

	smbfs_setsize(np->n_vnode, new_size);
	return TRUE;
}

int
smbfs_update_name_par(struct smb_share *share, vnode_t dvp, vnode_t vp,
                      struct timespec *reqtime,
                      const char *new_name, size_t name_len)
{
    char *new_name2, *old_name;
    struct componentname cnp;
    struct smbnode *np, *fdnp = NULL, *tdnp = NULL;
    vnode_t fdvp = NULL;
    uint32_t orig_flag = 0;
    int update_flags = 0;
    int exclusive_lock = 0;

    if ((vp == NULL) ||
        (dvp == NULL) ||
        (share == NULL) ||
        (reqtime == NULL) ||
        (new_name == NULL)) {
        /* Nothing to update */
        //SMBDEBUG("missing info \n");
        return TRUE;
    }
    
    np = VTOSMB(vp);
    
    /* 
     * Did the parent change? 
     *
     * fdnp = np->n_parent
     * fdvp = np->n_parent->n_vnode (not locked)
     *
     * tdnp = VTOSMB(dvp)
     * tdvp = dvp (locked)
     *
     * fnp = np (vp is locked)
     */
    lck_rw_lock_shared(&np->n_parent_rwlock);

    if (np->n_parent != NULL) {
        fdnp = np->n_parent;
        if (fdnp->n_vnode != NULL) {
            fdvp = fdnp->n_vnode;
        }
    }

    /* Already checked earlier for dvp == null */
    tdnp = VTOSMB(dvp);
    
    if ((fdnp != NULL) &&
        (fdvp != NULL) &&
        (tdnp != NULL) &&
        (fdnp != tdnp)) {
        /*
         * Parent changed, so need exclusive lock. Try to upgrade lock.
         * If exclusive lock upgrade fails we lose the lock and
         * have to take the exclusive lock on our own.
         */
        if (lck_rw_lock_shared_to_exclusive(&np->n_parent_rwlock) == FALSE) {
            lck_rw_lock_exclusive(&np->n_parent_rwlock);
            
            /*
             * Its remotely possible n_parent changed as we were getting the
             * exclusive lock, so reset fdnp and fdvp
             */
            fdnp = NULL;
            fdvp = NULL;
            
            if (np->n_parent != NULL) {
                fdnp = np->n_parent;
                if (fdnp->n_vnode != NULL) {
                    fdvp = fdnp->n_vnode;
                }
            }
            
            /* Make sure fdnp and fdvp are still ok */
            if ((fdnp == NULL) || (fdvp == NULL)) {
                /* 
                 * The parent disappeared. This should not happen. 
                 * Just leave the vnode unchanged.
                 */
                SMBERROR_LOCK(np, "Parent lost during update for <%s> \n", np->n_name);
                exclusive_lock = 1;
                goto error;
            }
        }
        exclusive_lock = 1;
        
        orig_flag = np->n_flag;

        /* Take a ref count on the new parent */
        if (!vnode_isvroot(dvp)) {
            if (vnode_ref(dvp) == 0) {
                np->n_flag |= NREFPARENT;
                
                /* Increment new parent node's child refcnt */
                OSIncrementAtomic(&tdnp->n_child_refcnt);
            }
            else {
                /* Failed to take ref, so clear flag */
                np->n_flag &= ~NREFPARENT;
            }
        }
        else {
            /* Do not need to ref cnt if parent is root vnode */
            np->n_flag &= ~NREFPARENT;
        }
        
        /* 
         * Remove the ref count off the old parent if there was one and
         * if the old parent was not root vnode
         */
        if ((!vnode_isvroot(fdvp)) && (orig_flag & NREFPARENT)) {
            if (vnode_get(fdvp) == 0) {
                vnode_rele(fdvp);
                vnode_put(fdvp);
                
                /* Remove the child refcnt from old parent */
                OSDecrementAtomic(&fdnp->n_child_refcnt);
            }
        }
        
        /* Set the new parent */
        np->n_parent = VTOSMB(dvp);
        
        /* Mark that we need to update the vnodes parent */
        update_flags |= VNODE_UPDATE_PARENT;
    }

error:
    if (exclusive_lock == 0) {
        /* Most of the time we should end up with just a shared lock */
        lck_rw_unlock_shared(&np->n_parent_rwlock);
    }
    else {
        /* Parent must have changed */
        lck_rw_unlock_exclusive(&np->n_parent_rwlock);
    }

    /*
     * Did the name change?
     */
    lck_rw_lock_shared(&np->n_name_rwlock);
    if ((np->n_nmlen == name_len) &&
        (bcmp(np->n_name, new_name, np->n_nmlen) == 0)) {
        /* Name did not change, so nothing to update */
        
        /* Update parent if needed */
        if (update_flags != 0) {
            vnode_update_identity(vp, dvp, np->n_name, (int) np->n_nmlen, 0,
                                  update_flags);
        }
        
        lck_rw_unlock_shared(&np->n_name_rwlock);
        return TRUE;
    }
    lck_rw_unlock_shared(&np->n_name_rwlock);
    
    /*
     * n_rename_time is used to handle the case where an Enumerate req is sent,
     * then a Rename request/reply happens, then the Enumerate reply is 
     * processed which has the previous name. We dont want to update the name 
     * with a stale name from an Enumerate that happened before the Rename.
     */
	if (timespeccmp(reqtime, &np->n_rename_time, <=)) {
        /* we lost the race, tell the calling routine */

        /* Update parent if needed */
        if (update_flags != 0) {
            lck_rw_lock_shared(&np->n_name_rwlock);
            vnode_update_identity(vp, dvp, np->n_name, (int) np->n_nmlen, 0,
                                  update_flags);
            lck_rw_unlock_shared(&np->n_name_rwlock);
        }
        
        return FALSE;
	}

    /* Set the new name */
    new_name2 = smb_strndup(new_name, name_len);
    if (new_name2) {
        /* save the old name */
        lck_rw_lock_exclusive(&np->n_name_rwlock);
        old_name = np->n_name;
        
        /* put in the new name */
        np->n_name = new_name2;
        np->n_nmlen = name_len;
        
        /* Now its safe to free the old name */
        SMB_FREE(old_name, M_SMBNODENAME);

        /* Update the VFS name cache */
        bzero(&cnp, sizeof(cnp));
        cnp.cn_nameptr = (char *)np->n_name;
        cnp.cn_namelen = (int) np->n_nmlen;
        cnp.cn_flags = MAKEENTRY;
        
        /* Remove old entry, wrong case */
        cache_purge(vp);
        
        /* Add new entry, correct case */
        cache_enter(dvp, vp, &cnp);
        lck_rw_unlock_exclusive(&np->n_name_rwlock);
       
        update_flags |= VNODE_UPDATE_NAME;
    }

    /* Update parent and/or name if needed */
    if (update_flags != 0) {
        lck_rw_lock_shared(&np->n_name_rwlock);
        vnode_update_identity(vp, dvp, np->n_name, (int) np->n_nmlen, 0,
                              update_flags);
        lck_rw_unlock_shared(&np->n_name_rwlock);
    }

	return TRUE;
}

/*
 * FindByteRangeLockEntry
 *
 * Return Values
 *
 *	TRUE	- We have this ranged locked already
 *	FALSE	- We don't have this range locked
 */
int 
FindByteRangeLockEntry(struct fileRefEntry *fndEntry, int64_t offset, 
					int64_t length, uint32_t lck_pid)
{
	struct ByteRangeLockEntry *curr = fndEntry->lockList;
	
	while (curr) {
		if ((curr->offset == offset) && (curr->length == length) && 
			(curr->lck_pid == lck_pid))
			return TRUE;
		curr = curr->next;			
	}
	return FALSE;
}
	
/*
 * AddRemoveByteRangeLockEntry
 *
 * Add .
 * 
 * Return Values
 *	none
 */
void 
AddRemoveByteRangeLockEntry(struct fileRefEntry *fndEntry, int64_t offset, 
						 int64_t length, int8_t unLock, uint32_t lck_pid)
{
	struct ByteRangeLockEntry *curr = NULL;
	struct ByteRangeLockEntry *prev = NULL;
	struct ByteRangeLockEntry *new = NULL;
	int32_t foundIt = 0;

	if (unLock == 0) {	/* Locking, so add a new ByteRangeLockEntry */
		SMB_MALLOC (new, struct ByteRangeLockEntry *, sizeof (struct ByteRangeLockEntry), 
				M_TEMP, M_WAITOK);
		new->offset = offset;
		new->length = length;
		new->lck_pid = lck_pid;
		new->next = NULL;

		curr = fndEntry->lockList;
		if (curr == NULL) {
			/* first entry is empty so use it */
			fndEntry->lockList = new;
		} else { /* find the last entry and add the new entry to the end of list */
			while (curr->next != NULL)
				curr = curr->next;
			curr->next = new;
		}
	} else {	/* Unlocking, so remove a ByteRangeLockEntry */
		curr = fndEntry->lockList;
		if (curr == NULL) {
		    SMBWARNING("AddRemoveByteRangeLockEntry:  no entries found\n");
		    return;
		}
		
		if ((curr->offset == offset) && (curr->length == length)) {
			/* first entry is it, so remove it from the head */
			fndEntry->lockList = curr->next;
			SMB_FREE(curr, M_TEMP);
		} else {
			/* Not the first entry, so search the rest of them */
			prev = curr;
			curr = curr->next;
			while (curr != NULL) {
				if ((curr->offset == offset) && (curr->length == length)) {
					foundIt = 1;
					/* found it so remove it */
					prev->next = curr->next;
					SMB_FREE(curr, M_TEMP);
					break;
				}
				prev = curr;
				curr = curr->next;
			}

			if (foundIt == 0) {
				SMBWARNING ("offset 0x%llx/0x%llx not found in fndEntry %p\n", 
							offset, length, (void *)fndEntry);
			}
		}
	}
}

/*
 * AddFileRef
 *
 * Create a new open deny file list entry.
 * 
 * Return Values
 *	fndEntry is not NULL then return the entry.
 */
void 
AddFileRef(vnode_t vp, struct proc *p, uint16_t accessMode, uint32_t rights, 
           SMBFID fid, struct smb2_durable_handle dur_handle, struct fileRefEntry **fndEntry)
{
    struct smbnode	*np = VTOSMB(vp);
    struct fileRefEntry *entry = NULL;
    struct fileRefEntry *current = NULL;

    /* Create a new fileRefEntry and insert it into the hp list */
    SMB_MALLOC(entry, struct fileRefEntry *, sizeof (struct fileRefEntry),
               M_TEMP, M_WAITOK);
    entry->refcnt = 0;
    entry->mmapped = FALSE;
    entry->proc = p;
    entry->p_pid = proc_pid(p);
    entry->accessMode = accessMode;
    entry->rights = rights;
    entry->fid = fid;
    entry->lockList = NULL;
    entry->dur_handle = dur_handle;
    entry->next = NULL;

    if (fndEntry) {
        *fndEntry = entry;
    }

    lck_mtx_lock(&np->f_openDenyListLock);
    
    if (np->f_openDenyList == NULL) {
        /* No other entries, so we are the first */
        np->f_openDenyList = entry;
    }
    else { 
        /* look for last entry in the list */
        current = np->f_openDenyList;
        while (current->next != NULL) {
            current = current->next;
        }

        /* put it at the end of the list */
        current->next = entry;
    }
    
    lck_mtx_unlock(&np->f_openDenyListLock);
}

/*
 * FindFileEntryByFID
 *
 * Find an entry in the open deny file list entry. Use the fid to locate the
 * entry.
 * 
 * Return Values
 *	-1	No matching entry found
 *	0	Found a match 
 */
int32_t 
FindFileEntryByFID(vnode_t vp, SMBFID fid, struct fileRefEntry **fndEntry)
{
	struct fileRefEntry *entry = NULL;
	struct smbnode *np;
	
#ifdef SMB_DEBUG
	if (fndEntry)
		DBG_ASSERT(*fndEntry == NULL);
#endif // SMB_DEBUG
	
	/* If we have no vnode then we are done. */
	if (!vp) {
		return (-1);
    }

	np = VTOSMB(vp);
	lck_mtx_lock(&np->f_openDenyListLock);
	/* Now search the list until we find a match */
	for (entry = np->f_openDenyList; entry; entry = entry->next) {
		if (entry->fid == fid) {
			if (fndEntry) {
				*fndEntry = entry;
            }
			lck_mtx_unlock(&np->f_openDenyListLock);
			return(0);
		}
	}
	lck_mtx_unlock(&np->f_openDenyListLock);
	return(-1);	/* No match found */
}

/*
 * FindFileEntryByLeaseKey
 *
 * Find an entry in the open deny file list entry. Use the lease key to locate the
 * entry.
 *
 * Return Values
 *	-1	No matching entry found
 *	0	Found a match
 */
int32_t
FindFileEntryByLeaseKey(vnode_t vp, uint64_t lease_key_hi, uint64_t lease_key_low, struct fileRefEntry **fndEntry)
{
    struct fileRefEntry *entry = NULL;
    int32_t foundIt = FALSE;
    struct smbnode *np;

    /* If we have no vnode then we are done. */
    if (!vp) {
        SMBDEBUG("no vnode? \n");
		return (foundIt);
    }

    np = VTOSMB(vp);
    lck_mtx_lock(&np->f_openDenyListLock);
    
    for (entry = np->f_openDenyList; entry; entry = entry->next) {
        if ((entry->dur_handle.lease_key_hi == lease_key_hi) &&
            (entry->dur_handle.lease_key_low == lease_key_low)) {
            if (fndEntry) {
                *fndEntry = entry;
            }
			foundIt = TRUE;
			break;
        }
    }
    
    lck_mtx_unlock(&np->f_openDenyListLock);
	return (foundIt);
}

/*
 * FindMappedFileRef
 *
 * Search the open deny file list entry looking for a mapped entry. If they
 * requested the entry return it, if they requested the fid return it also. 
 * 
 * Return Values
 *	FALSE	No matching entry found
 *	TRUE	Found a match
 */
int32_t 
FindMappedFileRef(vnode_t vp, struct fileRefEntry **fndEntry, SMBFID *fid)
{
	struct fileRefEntry *entry = NULL;
	int32_t foundIt = FALSE;
	struct smbnode	*np;
	
	/* If we have no vnode then we are done. */
	if (!vp) {
		return (foundIt);
    }
	
	np = VTOSMB(vp);
	lck_mtx_lock(&np->f_openDenyListLock);
	for (entry = np->f_openDenyList; entry; entry = entry->next) {
		if (entry->mmapped) {
			if (fid) {
			    *fid = entry->fid;
            }
			if (fndEntry) {
			    *fndEntry = entry;
            }
			foundIt = TRUE;
			break;
		}
	}
	lck_mtx_unlock(&np->f_openDenyListLock);
	return (foundIt);
}

/*
 * FindFileRef
 *
 * Find an entry in the open deny file list entry. Use accessMode and flags to 
 * locate the entry.
 * 
 * Return Values
 *	-1	No matching entry found
 *	0	Found a match
 *			if fndEntry is not NULL it will point to that entry.
 *			fid now holds file reference id for that entry.
 */
int32_t 
FindFileRef(vnode_t vp, proc_t p, uint16_t accessMode, int32_t flags, 
            int64_t offset, int64_t length, struct fileRefEntry **fndEntry,
            SMBFID *fid)
{
	struct fileRefEntry *entry = NULL;
	struct fileRefEntry *tempEntry = NULL;
	struct ByteRangeLockEntry *currBRL = NULL;
	int32_t foundIt = 0;
	struct smbnode	*np;
	
#ifdef SMB_DEBUG
	if (fndEntry)
		DBG_ASSERT(*fndEntry == NULL);
#endif // SMB_DEBUG
	/* If we have no vnode then we are done. */
	if (!vp) {
		return (-1);
    }

	np = VTOSMB(vp);
	lck_mtx_lock(&np->f_openDenyListLock);
	for (entry = np->f_openDenyList; entry; entry = entry->next) {
		/* 
		 * Remember that p can be NULL, but in that case this is coming from the
		 * kernel and is not associated with a particular proc. In fact it just 
		 * may be the pager itself trying to free up space and there is no proc. 
		 * I need to find any proc that already has the fork open for read or 
		 * write to use for read/write to work. So if no proc then just search
		 * the whole list and match on the first pid that matches the requested
		 * access.
		 */
		if ((p) && (entry->p_pid != proc_pid(p))) {
            SMBERROR("pid not matching \n");
			continue;
        }
		
		switch (flags) {
            case kPreflightOpen:
                /* 
                 * Look for denials locally first
                 * 1) If there is an existing exclusive open, then no other opens
                 * are allowed. 
                 * 2) If there is an existing shared open (denyWrite), then no 
                 * other opens for write are allowed 
                 * 3) If there is an existing open with Write, then no other
                 * opens for denyWrite are allowed
                 */
                if ((entry->accessMode & kDenyRead) &&
                    (entry->accessMode & kDenyWrite)) {
                    foundIt = 1;
                }
                
                if ((entry->accessMode & kDenyWrite) &&
                    (accessMode & kAccessWrite)) {
                    foundIt = 1;
                }

                if ((entry->accessMode & kAccessWrite) &&
                    (accessMode & kDenyWrite)) {
                    foundIt = 1;
                }
               break;
            case kAnyMatch:
                /* 
                 * if any fork will do, make sure at least have accessMode 
                 * set. This is for the old ByteRangeLocks and other misc 
                 * functions looking for a file ref 
                 */
                if (entry->accessMode & accessMode) {
                    foundIt = 1;
                }
                break;
            case kCheckDenyOrLocks:
                /* 
                 * This was originally written for Classic support, but after looking
                 *  at it some we decide it could happen in Carbon.
                 *
                 * Where I have the same PID on two different file, some BRL taken, 
                 * and a read/write occurring. I have to determine which file will 
                 * successfully read/write on due to any possible byte range locks 
                 * already taken out.  Note that Classic keeps track of BRLs itself 
                 * and will not block any read/writes that would fail due to a BRL.  
                 * I just have to find the correct fork so that the read/write will 
                 * succeed. Example:  open1 rw/DW, open2 r, lock1 0-5, read1 0-5 
                 * should occur on fork1 and not fork2
                */
                /* make sure we have correct access */
                if (entry->accessMode & accessMode) {
                    /* 
                     * save this entry in case we find no entry with a matching BRL.
                     * saves me from having to search all over again for an OpenDeny match 
                     */
                    if (tempEntry == NULL) {
                        tempEntry = entry;
                    }

                    /* check the BRLs to see if the offset/length fall inside one of them */
                    currBRL = entry->lockList;
                    while (currBRL != NULL) {
                        /* is start of read/write inside of the BRL? */
                        if ( (offset >= currBRL->offset) && 
                            (offset <= (currBRL->offset + currBRL->length)) ) {
                            foundIt = 1;
                            break;
                        }
                        /* is end of read/write inside of the BRL? */
                        if ( ((offset + length) >= currBRL->offset) &&  
                            ((offset + length) <= (currBRL->offset + currBRL->length)) ) {
                            foundIt = 1;
                            break;
                        }
                        currBRL = currBRL->next;
                    }
                }
                break;
                
            case kExactMatch:
            default:
                /* 
                 * If we want an exact match, then check access mode too
                 * This is for ByteRangeLocks and closing files 
                 */
                if (accessMode == entry->accessMode) {
                    foundIt = 1;
                }
                else {
                    /*
                     * In OS 9.x, if you opened a file for read only and it 
                     * failed, and there was a file opened already for 
                     * read/write, then open worked. Weird. For X, if first 
                     * open was r/w/dR/dW, r/w/dW, r/dR/dW, or r/dW, then a 
                     * second open from same pid asking for r/dR/dW or r/dW  
                     * will be allowed.
                     *
                     * See Radar 5050120, 11024374 for an example of this 
                     * happening.
                     *
                     * For kExactMatch, we know for sure either dW or dR/dW was
                     * done.
                     */
                    if ((accessMode & kDenyWrite) &&
                        (accessMode & kAccessRead) &&
                        !(accessMode & kAccessWrite)) {
                        /* We are looking for a match for r/dR/dW or r/dW */
                        if (entry->accessMode & kAccessRead) {
                            /* 
                             * existing entry has read, so has to have been 
                             * r/w/dR/dW, r/w/dW, r/dR/dW, or r/dW and thus
                             * this is a match
                             */
                            foundIt = 1;
                        }
                    }
                }
                break;
		}
        
		if (foundIt == 1) {
			*fid = entry->fid;
			if (fndEntry) {
                *fndEntry = entry;
            }
			break;
		}
	}
	lck_mtx_unlock(&np->f_openDenyListLock);

	/* Will only happen after we add byte range locking support */
	if (foundIt == 0) {
		if ( (flags == kCheckDenyOrLocks) && (tempEntry != NULL) ) {
			/* 
             * Did not find any BRL that matched, see if there was a match 
             * with an OpenDeny 
             */
			*fid = tempEntry->fid;
			if (fndEntry) {
                *fndEntry = entry;
            }
			return (0);
		}
		return (-1);    /* fork not found */
	}
	else
		return (0);
}

/*
 * RemoveFileRef
 *
 * Remove the entry that was passed in from the list and free it. If no entry is 
 * passed in then remove all entries.
 * 
 * Return Values
 *	none
 */
void 
RemoveFileRef(vnode_t vp, struct fileRefEntry *inEntry)
{
	struct smbnode	*np = VTOSMB(vp);
	struct fileRefEntry *curr = NULL;
	struct fileRefEntry *prev = NULL;
	struct fileRefEntry *entry = NULL;
	struct ByteRangeLockEntry *currBRL = NULL;
	struct ByteRangeLockEntry *nextBRL = NULL;
	int32_t foundIt = 0;

	lck_mtx_lock(&np->f_openDenyListLock);
	if (inEntry == NULL) {	/* Means remove all */
		entry = np->f_openDenyList;
		while (entry != NULL) {
			/* wipe out the ByteRangeLockEntries first */
			currBRL = entry->lockList;
			while (currBRL != NULL) {
				nextBRL = currBRL->next; /* save next in list */
				SMB_FREE (currBRL, M_TEMP);	 /* free current entry */
				currBRL = nextBRL;	 /* and on to the next */
			}
			entry->lockList = NULL;
			/* now wipe out the file refs */
			curr = entry;
			entry = entry->next;
			DBG_ASSERT(curr->refcnt == 0);
			SMB_FREE(curr, M_TEMP);
		}
		np->f_openDenyList = NULL;
		goto out;
	}
	DBG_ASSERT(inEntry->refcnt == 0);

	/* wipe out the ByteRangeLockEntries first */
	currBRL = inEntry->lockList;
	while (currBRL != NULL) {
		nextBRL = currBRL->next;	/* save next in list */
		SMB_FREE(currBRL, M_TEMP);		/* free current entry */
		currBRL = nextBRL;		/* and on to the next */
	}
	inEntry->lockList = NULL;

	/* Remove the fileRefEntry */
	curr = np->f_openDenyList;
	if (curr == NULL)
		goto out;
	/* 
	 * if its the first entry in the list, then just set the first 
	 * entry to be entry->next
	 */
	if (inEntry == curr) {
		np->f_openDenyList = inEntry->next;
		foundIt = 1;
		SMB_FREE(curr, M_TEMP);
		curr = NULL;
	} else {
		// its not the first, so search the rest
		prev = np->f_openDenyList;
		curr = prev->next;
		while (curr != NULL) {
			if (inEntry == curr) {
				prev->next = curr->next;
				foundIt = 1;
				SMB_FREE(curr, M_TEMP);
				curr = NULL;
				break;
			}
			prev = curr;
			curr = curr->next;
		}
	}
	if (foundIt == 0)
		SMBWARNING ("inEntry %p not found in vp %p\n", (void *)inEntry, (void *)vp);
out:
	lck_mtx_unlock(&np->f_openDenyListLock);
}

static void
smb1fs_reconnect(struct smbmount *smp)
{
    struct smbnode *np;
    uint32_t ii;
    
    /* Get the hash lock */
    smbfs_hash_lock(smp);
    
    /* We have a hash table for each mount point */
    for (ii = 0; ii < (smp->sm_hashlen + 1); ii++) {
        if ((&smp->sm_hash[ii])->lh_first == NULL)
            continue;
        
        for (np = (&smp->sm_hash[ii])->lh_first; np; np = np->n_hash.le_next) {
            if (ISSET(np->n_flag, NALLOC))
                continue;
            
            if (ISSET(np->n_flag, NTRANSIT))
                continue;
			
            /*
             * Someone is monitoring this item and we reconnected. Force a
             * notify update.
             */
            if (np->n_vnode && (vnode_ismonitored(np->n_vnode))) {
                SMBDEBUG_LOCK(np, "%s needs to be updated.\n", np->n_name);

                /* Do we need to reopen this item */
                if ((np->n_dosattr & SMB_EFA_DIRECTORY) && (np->d_fid != 0)) {
                    np->d_needReopen = TRUE;
                }
                
                /* Force a network lookup */
                np->attribute_cache_timer = 0;
                np->n_symlink_cache_timer = 0;
                np->d_needsUpdate = TRUE;
            }
            
            /* Nothing else to do with directories at this point */
            if (np->n_dosattr & SMB_EFA_DIRECTORY) {
                continue;
            }
            
            /* We only care about open files */
            if (np->f_refcnt == 0) {
                continue;
            }
            
            /*
             * We have an open file mark it to be reopen.
             *
             * 1. Plain old POSIX open with no locks. Only revoke if reopen fails.
             * 2. POSIX open with a flock. Revoke if reopen fails. Otherwise
             *	  reestablish the lock. If the lock fails then mark it to be revoked.
             * 3. POSIX open with POSIX locks. (We do not support posix locks)
             * 4. Shared or Exclusive OpenDeny . We now revoke always.
             * 5. Carbon Mandatory Locks. We now revoke always.
             */
            lck_mtx_lock(&np->f_openStateLock);
            
            /* Once it has been revoked it stays revoked */
            if (!(np->f_openState & kNeedRevoke))	{
                if (np->f_openDenyList) {
                    /*
                     * We always revoke opens that have mandatory
                     * locks or deny modes
                     */
                    np->f_openState |= kNeedRevoke;
                }
                else {
                    /* Reopen lazily later */
                    np->f_openState |= kNeedReopen;
                }
            }
            
            lck_mtx_unlock(&np->f_openStateLock);
        }
    }
    
    smbfs_hash_unlock(smp);
}

static void
smb2fs_reconnect(struct smbmount *smp)
{
    struct smbnode *np;
    uint32_t ii;
    struct smbfattr *fap = NULL;
    struct smb_vc *vcp;
	struct fileRefEntry *current = NULL;
    int error;
    SMB2FID temp_fid;
    uint32_t need_reopen = 0, done;

    vcp = SSTOVC(smp->sm_share);

    SMB_MALLOC(fap,
               struct smbfattr *,
               sizeof(struct smbfattr),
               M_SMBTEMP,
               M_WAITOK | M_ZERO);
    
    /* Attempt to resend AAPL create context */
    if ((smp->sm_rvp != NULL) &&
        (vcp->vc_misc_flags & SMBV_OSX_SERVER)) {
        SMBDEBUG("Clearing OS X server flags\n");
        vcp->vc_misc_flags &= ~(SMBV_OSX_SERVER | SMBV_OTHER_SERVER);
        
        if (smp->sm_args.altflags & SMBFS_MNT_AAPL_OFF) {
            /* Turn off AAPL */
            SMBWARNING("AAPL has been turned off for %s volume\n",
                       (smp->sm_args.volume_name) ? smp->sm_args.volume_name : "");
            vcp->vc_misc_flags |= SMBV_OTHER_SERVER;
        }
        else {
            /*
             * Use iod_context so we can tell this is from reconnect
             * Share was locked from smb_iod_reconnect, so have to unlock it
             * otherwise we can deadlock in iod code when the share lock is
             * attempted to be locked again.
             */
            lck_mtx_unlock(&smp->sm_share->ss_shlock);
            
            /* Send a Create/Close */
            smb2fs_smb_cmpd_create(smp->sm_share, VTOSMB(smp->sm_rvp),
                                   NULL, 0,
                                   NULL, 0,
                                   SMB2_FILE_READ_ATTRIBUTES | SMB2_SYNCHRONIZE, VDIR,
                                   NTCREATEX_SHARE_ACCESS_ALL, FILE_OPEN,
                                   SMB2_CREATE_AAPL_QUERY, NULL,
                                   NULL, fap,
                                   NULL, vcp->vc_iod->iod_context);
            
            lck_mtx_lock(&smp->sm_share->ss_shlock);
        }
    }

    /*
     * <13934847> We can not hold the hash lock while we reopen files as
     * we end up dead locked. First go through the entire list with the 
     * hash lock and just mark the vnodes that need to be reopened with the
     * kNeedReopen flag.
     */
    
    /* Get the hash lock */
    smbfs_hash_lock(smp);
    
    /* We have a hash table for each mount point */
    for (ii = 0; ii < (smp->sm_hashlen + 1); ii++) {
        if ((&smp->sm_hash[ii])->lh_first == NULL)
            continue;
        
        for (np = (&smp->sm_hash[ii])->lh_first; np; np = np->n_hash.le_next) {
            if (ISSET(np->n_flag, NALLOC))
                continue;
            
            if (ISSET(np->n_flag, NTRANSIT))
                continue;
			
            /*
             * Someone is monitoring this item and we reconnected. Force a
             * notify update.
             */
            if (np->n_vnode && (vnode_ismonitored(np->n_vnode))) {
                SMBDEBUG_LOCK(np, "%s needs to be updated.\n", np->n_name);
                
                /* Do we need to reopen this item */
                if ((np->n_dosattr & SMB_EFA_DIRECTORY) && (np->d_fid != 0)) {
                    np->d_needReopen = TRUE;
                    
                    /* Remove the open fid from the fid table */
                    smb_fid_get_kernel_fid(smp->sm_share, np->d_fid,
                                           1, &temp_fid);
                }
                
                /* Force a network lookup */
                np->attribute_cache_timer = 0;
                np->n_symlink_cache_timer = 0;
                np->d_needsUpdate = TRUE;
            }

            if (np->n_dosattr & SMB_EFA_DIRECTORY) {
                if (np->d_fctx != NULL) {
                    /* Enumeration open dir is now closed, lazily reopen it */
                    np->d_fctx->f_need_close = FALSE;
                    
                    /* Remove the open fid from the fid table */
                    smb_fid_get_kernel_fid(smp->sm_share,
                                           np->d_fctx->f_create_fid,
                                           1, &temp_fid);
                }
                
                /* Nothing else to do with directories at this point */
                continue;
            }
            
            /*
             * Only files from here on
             */
            
            if (np->f_refcnt == 0) {
                /* No open files, so done with this file */
                continue;
            }
            
            /* Once it has been revoked it stays revoked */
            lck_mtx_lock(&np->f_openStateLock);
            if (np->f_openState & kNeedRevoke)	{
                lck_mtx_unlock(&np->f_openStateLock);
                continue;
            }
            else {
                /* Will try to reopen the files */
                np->f_openState |= kNeedReopen;
                
                /* Mark that at least one file needs to be reopened */
                need_reopen = 1;
            }
            lck_mtx_unlock(&np->f_openStateLock);
        } /* for np loop */
    } /* for ii loop */
    
    /* Free the hash lock */
    smbfs_hash_unlock(smp);
        
    if (need_reopen == 0) {
        /* No files need to be reopened, so leave */
        goto exit;
    }

    /*
     * <13934847> We can not hold the hash lock while we reopen files as
     * we end up dead locked. Now go through the list again holding the hash
     * lock and if a vnode needs to be reopened, drop the hash lock, clear the
     * kNeedReopen, attempt to reopen the vnode, then start at begining of
     * loop again until there are no more vnodes that need to be reopened.
     */
    done = 0;
    
    while (done == 0) {
        /* Assume there are no files to be reopened */
        done = 1;
        
        /* Get the hash lock */
        smbfs_hash_lock(smp);

        /* We have a hash table for each mount point */
        for (ii = 0; ii < (smp->sm_hashlen + 1); ii++) {
            if ((&smp->sm_hash[ii])->lh_first == NULL)
                continue;
            
            for (np = (&smp->sm_hash[ii])->lh_first; np; np = np->n_hash.le_next) {
                if (ISSET(np->n_flag, NALLOC))
                    continue;
                
                if (ISSET(np->n_flag, NTRANSIT))
                    continue;
                
                if (np->n_dosattr & SMB_EFA_DIRECTORY) {
                    continue;
                }
                
                /* Once it has been revoked it stays revoked */
                lck_mtx_lock(&np->f_openStateLock);
                if (np->f_openState & kNeedReopen) {
                    /*
                     * Need to reopen this file. Clear kNeedReopen state, this 
                     * way we know if a reconnect happened during reopen.  Set 
                     * kInReopen so smbfs_attr_cacheenter() will not be called.
                     */
                    np->f_openState &= ~kNeedReopen;
                    np->f_openState |= kInReopen;
                    lck_mtx_unlock(&np->f_openStateLock);
                }
                else {
                    /* This file does not need to be reopened */
                    lck_mtx_unlock(&np->f_openStateLock);
                    continue;
                }
                
                /* 
                 * Free the hash lock - this is why we have to redo entire 
                 * while loop as the hash table may now change.
                 */
                done = 0;
                smbfs_hash_unlock(smp);

                /*
                 * For all network calls, use iod_context so we can tell this is
                 * from reconnect and thus it wont get blocked waiting for credits.
                 *
                 * Share was locked from smb_iod_reconnect, so have to
                 * unlock it otherwise we can deadlock in iod code when
                 * the share lock is attempted to be locked again.
                 */
                
                /*
                 * Always check f_openDenyList too in case we need to remove all
                 * those fids too.
                 */
                
                error = 0;
                
                /*
                 * Reopen any fids on the f_openDenyList
                 *
                 * We should lock f_openDenyListLock but can not because we will
                 * deadlock
                 */
                if (np->f_openDenyList) {
                    current = np->f_openDenyList;
                    
                    do {
                        /*
                         * Any previous error will cause us to skip attempting to
                         * reopen rest of the fids and just close the fids instead.
                         */
                        if (error == 0) {
                            if (current->dur_handle.flags & SMB2_DURABLE_HANDLE_GRANTED) {
                                current->dur_handle.flags |= SMB2_DURABLE_HANDLE_RECONNECT;
                                current->dur_handle.flags &= ~(SMB2_DURABLE_HANDLE_GRANTED |
                                                               SMB2_LEASE_GRANTED);
                                current->dur_handle.fid = current->fid;
                                
                                lck_mtx_unlock(&smp->sm_share->ss_shlock);
                                error = smbfs_smb_ntcreatex(smp->sm_share, np,
                                                            0, 0, VREG,
                                                            &current->fid, NULL, 0,
                                                            0, FALSE, fap,
                                                            FALSE, &current->dur_handle, vcp->vc_iod->iod_context);
                                lck_mtx_lock(&smp->sm_share->ss_shlock);
                                if (error) {
                                    SMBERROR_LOCK(np, "Warning: Could not reopen %s \n", np->n_name);
                                }
                            }
                            else {
                                /*
                                 * Failed to get a durable handle when this file
                                 * was opened, so can not reopen this file
                                 */
                                SMBERROR_LOCK(np, "Missing durable handle %s \n", np->n_name);
                                error = EBADF;
                            }
                        }
                        
                        if (error) {
                            /* Remove the open fid from the fid table */
                            smb_fid_get_kernel_fid(smp->sm_share, current->fid,
                                                   1, &temp_fid);
                        }
                        
                        /* On to next fid */
                        current = current->next;
                    } while (current != NULL);
                    
                    lck_mtx_lock(&np->f_openStateLock);
                    
                    if (error) {
                        /* Mark the file as revoked */
                        np->f_openState |= kNeedRevoke;
                    } else if (np->f_fid == 0) {
                        /* No shared forks to open, we can clear kInReopen now */
                        np->f_openState &= ~kInReopen;
                    }
                    
                    lck_mtx_unlock(&np->f_openStateLock);
                }
                
                /*
                 * Reopen shared fork if one is present. Do this AFTER doing the
                 * f_openDenyList so we dont break any Handle leases
                 */
                if (np->f_fid != 0) {
                    /* Only reopen if no error from open deny list opens */
                    if (error == 0) {
                        lck_mtx_unlock(&smp->sm_share->ss_shlock);
                        error = smbfs_smb_reopen_file(smp->sm_share, np,
                                                      vcp->vc_iod->iod_context);
                        /* 
                         * smbfs_smb_reopen_file() sets the correct f_openState
                         * for us 
                         */
                        lck_mtx_lock(&smp->sm_share->ss_shlock);
                    }
                    
                    if (error) {
                        /*
                         * On failure, file is marked for revoke so we are done
                         * Remove the open fid from the fid table
                         */
                        smb_fid_get_kernel_fid(smp->sm_share, np->f_fid,
                                               1, &temp_fid);
                    }
                }
                
                /* 
                 * Paranoid check - its possible that we get reconnected while
                 * we are trying to reopen and that would reset the kInReopen
                 * which could keep us looping forever. For now, we will only
                 * try once to reopen a file and thats it. May have to rethink
                 * this if it becomes a problem.
                 */
                lck_mtx_lock(&np->f_openStateLock);
                
                if (np->f_openState & kNeedReopen) {
                    SMBERROR_LOCK(np, "Only one attempt to reopen %s \n", np->n_name);
                    np->f_openState &= ~kNeedReopen;
                    
                    /* Mark the file as revoked */
                    np->f_openState |= kNeedRevoke;
                }
                
                lck_mtx_unlock(&np->f_openStateLock);
                
                /* 
                 * Since we dropped the hash lock, have to start the while 
                 * loop again to search entire hash table from beginning.
                 */
                goto loop_again; /* skip out of np and ii loops */
                
            } /* for np loop */
        } /* for ii loop */
        
loop_again:
        
        if (done == 1) {
            /* if we get here, then must not have found any files to reopen */
            smbfs_hash_unlock(smp);
        }
    }
    
exit:
    if (fap) {
        SMB_FREE(fap, M_SMBTEMP);
    }
}

/*
 * The share needs to be locked before calling this routine!
 *
 * Search the hash table looking for any open files. Remember we have a hash
 * table for every mount point. Not sure why but it makes this part easier.
 * Currently we do not support reopens, we just mark the file to be revoked.
 */
void
smbfs_reconnect(struct smbmount *smp)
{
   	struct smb_vc *vcp;
    
	KASSERT(smb != NULL, ("smp is null"));
    
    vcp = SSTOVC(smp->sm_share);
	KASSERT(vcp != NULL, ("vcp is null"));

    if (vcp->vc_flags & SMBV_SMB2) {
        smb2fs_reconnect(smp);
    }
    else {
        smb1fs_reconnect(smp);
    }
}

/*
 * The share needs to be locked before calling this routine!
 *
 * Search the hash table looking for any open for write files or any files that
 * have dirty bits in UBC. If any are found, return EBUSY, else return 0.
 */
int32_t
smbfs_IObusy(struct smbmount *smp)
{
	struct smbnode *np;
	uint32_t ii;
	
    /* lock hash table before we walk it */
	smbfs_hash_lock(smp);
    
	/* We have a hash table for each mount point */
	for (ii = 0; ii < (smp->sm_hashlen + 1); ii++) {
		if ((&smp->sm_hash[ii])->lh_first == NULL)
			continue;
		
		for (np = (&smp->sm_hash[ii])->lh_first; np; np = np->n_hash.le_next) {
			if (ISSET(np->n_flag, NALLOC))
				continue;
            
			if (ISSET(np->n_flag, NTRANSIT))
				continue;

			/* Nothing else to with directories at this point */
			if (np->n_dosattr & SMB_EFA_DIRECTORY) {
				continue;
			}
			/* We only care about open files */			
			if (np->f_refcnt == 0) {
				continue;
			}
            
			if ((np->f_openTotalWCnt > 0) || (vnode_hasdirtyblks(SMBTOV(np)))) {
                /* Found oen busy file so return EBUSY */
                smbfs_hash_unlock(smp);
				return EBUSY;
			}
		}
	}
    
	smbfs_hash_unlock(smp);
    
    /* No files open for write and no files with dirty UBC data */
    return 0;
}

void
smbfs_ClearChildren(struct smbmount *smp, struct smbnode *parent)
{
    struct smbnode *np;
    uint32_t ii;

    /* lock hash table before we walk it */
    smbfs_hash_lock(smp);

    /* We have a hash table for each mount point */
    for (ii = 0; ii < (smp->sm_hashlen + 1); ii++) {
        if ((&smp->sm_hash[ii])->lh_first == NULL)
            continue;

        for (np = (&smp->sm_hash[ii])->lh_first; np; np = np->n_hash.le_next) {
            lck_rw_lock_exclusive(&np->n_parent_rwlock);
            
            if (np->n_parent == parent) {
                if (ISSET(np->n_flag, NALLOC)) {
                    /*
                     * Now if (np->n_parent == parent) : OOPS
                     *
                     * Parent is in reclaim and child in alloc.
                     * Most likely, it is the case of force unmount but we 
                     * should have never come here i.e. SMB should never create 
                     * a new child smbnode when its parent is in reclaim. In 
                     * fact, this can be verified by the fact that every 
                     * function (vfs ops) calling smbfs_nget() and 
                     * smbfs_vgetstrm() takes an exclusive lock on the parent. 
                     * So while in NALLOC, parent can't proceed in 
                     * smbfs_vnop_reclaim() since he would wait on this lock at 
                     * the very beginning. Looking at the code, it makes no 
                     * sense that we could ever hit this situation.
                     * Fixed in <rdar://problem/12442700>.
                     */
                 	SMBERROR("%s : Allocating child smbnode when parent \
                             is in reclaim\n", __FUNCTION__);
                }

                if (ISSET(np->n_flag, NTRANSIT)) {
                    /*
                     * Now if (np->n_parent == parent) : OOPS
                     *
                     * Parent is in reclaim and child in reclaim too.
                     * Most likely, tt is the case of force unmount but we 
                     * should have never come here i.e. SMB should never 
                     * reclaim a child smbnode when the parent is still in 
                     * reclaim. Looking at the code in smbfs_vnop_reclaim(), 
                     * parent can't acquire sm_reclaim_lock and call 
                     * smbfs_ClearChildren() if the child is already in 
                     * NTRANSIT since the child has sm_reclaim_lock lock.
                     * So even in case of force unmount, EITHER parent can be 
                     * here and child is yet to enter NTRANSIT OR the child has 
                     * this lock and is in NTRANSIT.
                     * Fixed in <rdar://problem/12442700>.
                     */
                	SMBERROR("%s : Child smbnode is in reclaim when parent \
                             is still in reclaim\n", __FUNCTION__);
                }

                /* Clear the parent reference for this child */
                np->n_flag &= ~NREFPARENT;
                np->n_parent = NULL;
            }
            
            lck_rw_unlock_exclusive(&np->n_parent_rwlock);
         }
    }
    
    smbfs_hash_unlock(smp);
}

int
smbfs_handle_lease_break(struct smbmount *smp, uint64_t lease_key_hi,
                         uint64_t lease_key_low, uint32_t new_lease_state)
{
    int error = 0;
    uint32_t tree_id = 0;
    uint64_t hash_val = 0;
	vnode_t	vp = NULL;
	struct fileRefEntry *entry;
	struct smbnode_hashhead	*nhpp;
	struct smbnode *np;
	uint32_t vid;

    /* Get hash value from lease key */
    smb2_smb_dur_handle_parse_lease_key(lease_key_hi, lease_key_low,
                                        &tree_id, &hash_val);

    /* 
     * Server must support File IDs as we have no name/name_len to use.
     * Find vnode using hash value, but SKIP locking it!
     * Deadlock happens when you have file A open with durable handle, then
     * another process opens file A after taking a node lock. The open request 
     * goes to server which generates a lease break on file A. If you try to
     * take a node lock in processing the lease break, you end up deadlocked.
     */
loop:
	smbfs_hash_lock(smp);
    
	nhpp = SMBFS_NOHASH(smp, hash_val);
	LIST_FOREACH(np, nhpp, n_hash) {
        if (np->n_ino != hash_val) {
            continue;
        }
        
		if (ISSET(np->n_flag, NALLOC)) {
			SET(np->n_flag, NWALLOC);
			(void) msleep((caddr_t)np, smp->sm_hashlock, PINOD | PDROP, "smb_ngetalloc", 0);
			goto loop;
		}
        
		if (ISSET(np->n_flag, NTRANSIT)) {
			SET(np->n_flag, NWTRANSIT);
			(void) msleep((caddr_t)np, smp->sm_hashlock, PINOD | PDROP, "smb_ngettransit", 0);
			goto loop;
		}
        
        /* 
         * Found a match, get the vnode
         */
		vp = SMBTOV(np);
		vid = vnode_vid(vp);

		if (vnode_getwithvid(vp, vid)) {
            /* Failed to get vnode */
			continue;
        }
        
        /* See if this vnode has the file ref entry that matches lease key */
        if (FindFileEntryByLeaseKey(vp, lease_key_hi, lease_key_low, &entry) == TRUE) {
            /*
             * At this time, we do nothing with the lease, its just used for
             * getting durable handles. Later when we actually use the leases for
             * local caching, the lease break handling code should be moved to the
             * change notify thread instead of using the iod thread.
             */
            entry->dur_handle.lease_state = new_lease_state;
            error = 0;
            vnode_put(vp);
            break;
        }
        else {
            SMBERROR("No fileRefEntry found for lease break \n");
            vnode_put(vp);
            continue;
        }
	}
    
	smbfs_hash_unlock(smp);

    return (error);
}


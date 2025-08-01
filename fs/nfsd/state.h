/*
 *  Copyright (c) 2001 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Kendrick Smith <kmsmith@umich.edu>
 *  Andy Adamson <andros@umich.edu>
 *  
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *  
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the University nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 *  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef _NFSD4_STATE_H
#define _NFSD4_STATE_H

#include <linux/idr.h>
#include <linux/refcount.h>
#include <linux/sunrpc/svc_xprt.h>
#include "nfsfh.h"
#include "nfsd.h"

typedef struct {
	u32             cl_boot;
	u32             cl_id;
} clientid_t;

typedef struct {
	clientid_t	so_clid;
	u32		so_id;
} stateid_opaque_t;

typedef struct {
	u32                     si_generation;
	stateid_opaque_t        si_opaque;
} stateid_t;

typedef struct {
	stateid_t		cs_stid;
#define NFS4_COPY_STID 1
#define NFS4_COPYNOTIFY_STID 2
	unsigned char		cs_type;
	refcount_t		cs_count;
} copy_stateid_t;

struct nfsd4_referring_call {
	struct list_head	__list;

	u32			rc_sequenceid;
	u32			rc_slotid;
};

struct nfsd4_referring_call_list {
	struct list_head	__list;

	struct nfs4_sessionid	rcl_sessionid;
	int			__nr_referring_calls;
	struct list_head	rcl_referring_calls;
};

struct nfsd4_callback {
	struct nfs4_client *cb_clp;
	struct rpc_message cb_msg;
#define NFSD4_CALLBACK_RUNNING		(0)
#define NFSD4_CALLBACK_WAKE		(1)
#define NFSD4_CALLBACK_REQUEUE		(2)
	unsigned long cb_flags;
	const struct nfsd4_callback_ops *cb_ops;
	struct work_struct cb_work;
	int cb_seq_status;
	int cb_status;
	int cb_held_slot;

	int cb_nr_referring_call_list;
	struct list_head cb_referring_call_list;
};

struct nfsd4_callback_ops {
	void (*prepare)(struct nfsd4_callback *);
	int (*done)(struct nfsd4_callback *, struct rpc_task *);
	void (*release)(struct nfsd4_callback *);
	uint32_t opcode;
};

/*
 * A core object that represents a "common" stateid. These are generally
 * embedded within the different (more specific) stateid objects and contain
 * fields that are of general use to any stateid.
 */
struct nfs4_stid {
	refcount_t		sc_count;

	/* A new stateid is added to the cl_stateids idr early before it
	 * is fully initialised.  Its sc_type is then zero.  After
	 * initialisation the sc_type it set under cl_lock, and then
	 * never changes.
	 */
#define SC_TYPE_OPEN		BIT(0)
#define SC_TYPE_LOCK		BIT(1)
#define SC_TYPE_DELEG		BIT(2)
#define SC_TYPE_LAYOUT		BIT(3)
	unsigned short		sc_type;

/* state_lock protects sc_status for delegation stateids.
 * ->cl_lock protects sc_status for open and lock stateids.
 * ->st_mutex also protect sc_status for open stateids.
 * ->ls_lock protects sc_status for layout stateids.
 */
/*
 * For an open stateid kept around *only* to process close replays.
 * For deleg stateid, kept in idr until last reference is dropped.
 */
#define SC_STATUS_CLOSED	BIT(0)
/* For a deleg stateid kept around only to process free_stateid's: */
#define SC_STATUS_REVOKED	BIT(1)
#define SC_STATUS_ADMIN_REVOKED	BIT(2)
#define SC_STATUS_FREEABLE	BIT(3)
#define SC_STATUS_FREED		BIT(4)
	unsigned short		sc_status;

	struct list_head	sc_cp_list;
	stateid_t		sc_stateid;
	spinlock_t		sc_lock;
	struct nfs4_client	*sc_client;
	struct nfs4_file	*sc_file;
	void			(*sc_free)(struct nfs4_stid *);
};

/* Keep a list of stateids issued by the COPY_NOTIFY, associate it with the
 * parent OPEN/LOCK/DELEG stateid.
 */
struct nfs4_cpntf_state {
	copy_stateid_t		cp_stateid;
	struct list_head	cp_list;	/* per parent nfs4_stid */
	stateid_t		cp_p_stateid;	/* copy of parent's stateid */
	clientid_t		cp_p_clid;	/* copy of parent's clid */
	time64_t		cpntf_time;	/* last time stateid used */
};

/*
 * RFC 7862 Section 4.8 states:
 *
 * | A copy offload stateid will be valid until either (A) the client
 * | or server restarts or (B) the client returns the resource by
 * | issuing an OFFLOAD_CANCEL operation or the client replies to a
 * | CB_OFFLOAD operation.
 *
 * Because a client might not reply to a CB_OFFLOAD, or a reply
 * might get lost due to connection loss, NFSD purges async copy
 * state after a short period to prevent it from accumulating
 * over time.
 */
#define NFSD_COPY_INITIAL_TTL 10

struct nfs4_cb_fattr {
	struct nfsd4_callback ncf_getattr;
	u32 ncf_cb_status;

	/* from CB_GETATTR reply */
	u64 ncf_cb_change;
	u64 ncf_cb_fsize;
	struct timespec64 ncf_cb_mtime;
	struct timespec64 ncf_cb_atime;

	bool ncf_file_modified;
	u64 ncf_initial_cinfo;
	u64 ncf_cur_fsize;
};

/*
 * Represents a delegation stateid. The nfs4_client holds references to these
 * and they are put when it is being destroyed or when the delegation is
 * returned by the client:
 *
 * o 1 reference as long as a delegation is still in force (taken when it's
 *   alloc'd, put when it's returned or revoked)
 *
 * o 1 reference as long as a recall rpc is in progress (taken when the lease
 *   is broken, put when the rpc exits)
 *
 * o 1 more ephemeral reference for each nfsd thread currently doing something
 *   with that delegation without holding the cl_lock
 *
 * If the server attempts to recall a delegation and the client doesn't do so
 * before a timeout, the server may also revoke the delegation. In that case,
 * the object will either be destroyed (v4.0) or moved to a per-client list of
 * revoked delegations (v4.1+).
 *
 * This object is a superset of the nfs4_stid.
 */
struct nfs4_delegation {
	struct nfs4_stid	dl_stid; /* must be first field */
	struct list_head	dl_perfile;
	struct list_head	dl_perclnt;
	struct list_head	dl_recall_lru;  /* delegation recalled */
	struct nfs4_clnt_odstate *dl_clnt_odstate;
	time64_t		dl_time;
	u32			dl_type;
/* For recall: */
	int			dl_retries;
	struct nfsd4_callback	dl_recall;
	bool			dl_recalled;

	/* for CB_GETATTR */
	struct nfs4_cb_fattr    dl_cb_fattr;
};

static inline bool deleg_is_read(u32 dl_type)
{
	return (dl_type == OPEN_DELEGATE_READ || dl_type == OPEN_DELEGATE_READ_ATTRS_DELEG);
}

static inline bool deleg_is_write(u32 dl_type)
{
	return (dl_type == OPEN_DELEGATE_WRITE || dl_type == OPEN_DELEGATE_WRITE_ATTRS_DELEG);
}

static inline bool deleg_attrs_deleg(u32 dl_type)
{
	return dl_type == OPEN_DELEGATE_READ_ATTRS_DELEG ||
	       dl_type == OPEN_DELEGATE_WRITE_ATTRS_DELEG;
}

#define cb_to_delegation(cb) \
	container_of(cb, struct nfs4_delegation, dl_recall)

/* client delegation callback info */
struct nfs4_cb_conn {
	/* SETCLIENTID info */
	struct sockaddr_storage	cb_addr;
	struct sockaddr_storage	cb_saddr;
	size_t			cb_addrlen;
	u32                     cb_prog; /* used only in 4.0 case;
					    per-session otherwise */
	u32                     cb_ident;	/* minorversion 0 only */
	struct svc_xprt		*cb_xprt;	/* minorversion 1 only */
};

static inline struct nfs4_delegation *delegstateid(struct nfs4_stid *s)
{
	return container_of(s, struct nfs4_delegation, dl_stid);
}

/* Maximum number of slots per session.  This is for sanity-check only.
 * It could be increased if we had a mechanism to shutdown misbehaving clients.
 * A large number can be needed to get good throughput on high-latency servers.
 */
#define NFSD_MAX_SLOTS_PER_SESSION	2048
/* Maximum  session per slot cache size */
#define NFSD_SLOT_CACHE_SIZE		2048
/* Maximum number of NFSD_SLOT_CACHE_SIZE slots per session */
#define NFSD_CACHE_SIZE_SLOTS_PER_SESSION	32
#define NFSD_MAX_MEM_PER_SESSION  \
		(NFSD_CACHE_SIZE_SLOTS_PER_SESSION * NFSD_SLOT_CACHE_SIZE)

struct nfsd4_slot {
	u32	sl_seqid;
	__be32	sl_status;
	struct svc_cred sl_cred;
	u32	sl_index;
	u32	sl_datalen;
	u16	sl_opcnt;
	u16	sl_generation;
#define NFSD4_SLOT_INUSE	(1 << 0)
#define NFSD4_SLOT_CACHETHIS	(1 << 1)
#define NFSD4_SLOT_INITIALIZED	(1 << 2)
#define NFSD4_SLOT_CACHED	(1 << 3)
#define NFSD4_SLOT_REUSED	(1 << 4)
	u8	sl_flags;
	char	sl_data[];
};

struct nfsd4_channel_attrs {
	u32		headerpadsz;
	u32		maxreq_sz;
	u32		maxresp_sz;
	u32		maxresp_cached;
	u32		maxops;
	u32		maxreqs;
	u32		nr_rdma_attrs;
	u32		rdma_attrs;
};

struct nfsd4_cb_sec {
	u32	flavor; /* (u32)(-1) used to mean "no valid flavor" */
	kuid_t	uid;
	kgid_t	gid;
};

struct nfsd4_create_session {
	clientid_t			clientid;
	struct nfs4_sessionid		sessionid;
	u32				seqid;
	u32				flags;
	struct nfsd4_channel_attrs	fore_channel;
	struct nfsd4_channel_attrs	back_channel;
	u32				callback_prog;
	struct nfsd4_cb_sec		cb_sec;
};

struct nfsd4_backchannel_ctl {
	u32	bc_cb_program;
	struct nfsd4_cb_sec		bc_cb_sec;
};

struct nfsd4_bind_conn_to_session {
	struct nfs4_sessionid		sessionid;
	u32				dir;
};

/* The single slot clientid cache structure */
struct nfsd4_clid_slot {
	u32				sl_seqid;
	__be32				sl_status;
	struct nfsd4_create_session	sl_cr_ses;
};

struct nfsd4_conn {
	struct list_head cn_persession;
	struct svc_xprt *cn_xprt;
	struct svc_xpt_user cn_xpt_user;
	struct nfsd4_session *cn_session;
/* CDFC4_FORE, CDFC4_BACK: */
	unsigned char cn_flags;
};

/* Maximum number of slots that nfsd will use in the backchannel */
#define NFSD_BC_SLOT_TABLE_SIZE		(sizeof(u32) * 8)

/*
 * Representation of a v4.1+ session. These are refcounted in a similar fashion
 * to the nfs4_client. References are only taken when the server is actively
 * working on the object (primarily during the processing of compounds).
 */
struct nfsd4_session {
	atomic_t		se_ref;
	spinlock_t		se_lock;
	u32			se_cb_slot_avail; /* bitmap of available slots */
	u32			se_cb_highest_slot;	/* highest slot client wants */
	u32			se_cb_prog;
	struct list_head	se_hash;	/* hash by sessionid */
	struct list_head	se_perclnt;
	struct list_head	se_all_sessions;/* global list of sessions */
	struct nfs4_client	*se_client;
	struct nfs4_sessionid	se_sessionid;
	struct nfsd4_channel_attrs se_fchannel;
	struct nfsd4_cb_sec	se_cb_sec;
	struct list_head	se_conns;
	u32			se_cb_seq_nr[NFSD_BC_SLOT_TABLE_SIZE];
	struct xarray		se_slots;	/* forward channel slots */
	u16			se_slot_gen;
	bool			se_dead;
	u32			se_target_maxslots;
};

/* formatted contents of nfs4_sessionid */
struct nfsd4_sessionid {
	clientid_t	clientid;
	u32		sequence;
	u32		reserved;
};

#define HEXDIR_LEN     33 /* hex version of 16 byte md5 of cl_name plus '\0' */

/*
 *       State                Meaning                  Where set
 * --------------------------------------------------------------------------
 * | NFSD4_ACTIVE      | Confirmed, active    | Default                     |
 * |------------------- ----------------------------------------------------|
 * | NFSD4_COURTESY    | Courtesy state.      | nfs4_get_client_reaplist    |
 * |                   | Lease/lock/share     |                             |
 * |                   | reservation conflict |                             |
 * |                   | can cause Courtesy   |                             |
 * |                   | client to be expired |                             |
 * |------------------------------------------------------------------------|
 * | NFSD4_EXPIRABLE   | Courtesy client to be| nfs4_laundromat             |
 * |                   | expired by Laundromat| try_to_expire_client        |
 * |                   | due to conflict      |                             |
 * |------------------------------------------------------------------------|
 */
enum {
	NFSD4_ACTIVE = 0,
	NFSD4_COURTESY,
	NFSD4_EXPIRABLE,
};

/*
 * struct nfs4_client - one per client.  Clientids live here.
 *
 * The initial object created by an NFS client using SETCLIENTID (for NFSv4.0)
 * or EXCHANGE_ID (for NFSv4.1+). These objects are refcounted and timestamped.
 * Each nfsd_net_ns object contains a set of these and they are tracked via
 * short and long form clientid. They are hashed and searched for under the
 * per-nfsd_net client_lock spinlock.
 *
 * References to it are only held during the processing of compounds, and in
 * certain other operations. In their "resting state" they have a refcount of
 * 0. If they are not renewed within a lease period, they become eligible for
 * destruction by the laundromat.
 *
 * These objects can also be destroyed if the client sends certain forms of
 * SETCLIENTID or EXCHANGE_ID operations.
 *
 * Care is taken *not* to do this however when the objects have an elevated
 * refcount.
 *
 * o Each nfs4_client is hashed by clientid
 *
 * o Each nfs4_clients is also hashed by name (the opaque quantity initially
 *   sent by the client to identify itself).
 *
 * o cl_perclient list is used to ensure no dangling stateowner references
 *   when we expire the nfs4_client
 */
struct nfs4_client {
	struct list_head	cl_idhash; 	/* hash by cl_clientid.id */
	struct rb_node		cl_namenode;	/* link into by-name trees */
	struct list_head	*cl_ownerstr_hashtbl;
	struct list_head	cl_openowners;
	struct idr		cl_stateids;	/* stateid lookup */
	struct list_head	cl_delegations;
	struct list_head	cl_revoked;	/* unacknowledged, revoked 4.1 state */
	struct list_head        cl_lru;         /* tail queue */
#ifdef CONFIG_NFSD_PNFS
	struct list_head	cl_lo_states;	/* outstanding layout states */
#endif
	struct xdr_netobj	cl_name; 	/* id generated by client */
	nfs4_verifier		cl_verifier; 	/* generated by client */
	time64_t		cl_time;	/* time of last lease renewal */
	struct sockaddr_storage	cl_addr; 	/* client ipaddress */
	bool			cl_mach_cred;	/* SP4_MACH_CRED in force */
	struct svc_cred		cl_cred; 	/* setclientid principal */
	clientid_t		cl_clientid;	/* generated by server */
	nfs4_verifier		cl_confirm;	/* generated by server */
	u32			cl_minorversion;
	atomic_t		cl_admin_revoked; /* count of admin-revoked states */
	/* NFSv4.1 client implementation id: */
	struct xdr_netobj	cl_nii_domain;
	struct xdr_netobj	cl_nii_name;
	struct timespec64	cl_nii_time;

	/* for v4.0 and v4.1 callbacks: */
	struct nfs4_cb_conn	cl_cb_conn;
#define NFSD4_CLIENT_CB_UPDATE		(0)
#define NFSD4_CLIENT_CB_KILL		(1)
#define NFSD4_CLIENT_STABLE		(2)	/* client on stable storage */
#define NFSD4_CLIENT_RECLAIM_COMPLETE	(3)	/* reclaim_complete done */
#define NFSD4_CLIENT_CONFIRMED		(4)	/* client is confirmed */
#define NFSD4_CLIENT_UPCALL_LOCK	(5)	/* upcall serialization */
#define NFSD4_CLIENT_CB_FLAG_MASK	(1 << NFSD4_CLIENT_CB_UPDATE | \
					 1 << NFSD4_CLIENT_CB_KILL)
	unsigned long		cl_flags;

	struct workqueue_struct *cl_callback_wq;
	const struct cred	*cl_cb_cred;
	struct rpc_clnt		*cl_cb_client;
	u32			cl_cb_ident;
#define NFSD4_CB_UP		0
#define NFSD4_CB_UNKNOWN	1
#define NFSD4_CB_DOWN		2
#define NFSD4_CB_FAULT		3
	int			cl_cb_state;
	struct nfsd4_callback	cl_cb_null;
	struct nfsd4_session	*cl_cb_session;

	/* for all client information that callback code might need: */
	spinlock_t		cl_lock;

	/* for nfs41 */
	struct list_head	cl_sessions;
	struct nfsd4_clid_slot	cl_cs_slot;	/* create_session slot */
	u32			cl_exchange_flags;
	/* number of rpc's in progress over an associated session: */
	atomic_t		cl_rpc_users;
	struct nfsdfs_client	cl_nfsdfs;
	struct nfs4_op_map      cl_spo_must_allow;

	/* debugging info directory under nfsd/clients/ : */
	struct dentry		*cl_nfsd_dentry;
	/* 'info' file within that directory. Ref is not counted,
	 * but will remain valid iff cl_nfsd_dentry != NULL
	 */
	struct dentry		*cl_nfsd_info_dentry;

	struct rpc_wait_queue	cl_cb_waitq;	/* backchannel callers may */
						/* wait here for slots */
	struct net		*net;
	struct list_head	async_copies;	/* list of async copies */
	spinlock_t		async_lock;	/* lock for async copies */
	atomic_t		cl_cb_inflight;	/* Outstanding callbacks */

	unsigned int		cl_state;
	atomic_t		cl_delegs_in_recall;

	struct nfsd4_cb_recall_any	*cl_ra;
	time64_t		cl_ra_time;
};

/* struct nfs4_client_reset
 * one per old client. Populates reset_str_hashtbl. Filled from conf_id_hashtbl
 * upon lease reset, or from upcall to state_daemon (to read in state
 * from non-volitile storage) upon reboot.
 */
struct nfs4_client_reclaim {
	struct list_head	cr_strhash;	/* hash by cr_name */
	struct nfs4_client	*cr_clp;	/* pointer to associated clp */
	struct xdr_netobj	cr_name;	/* recovery dir name */
	struct xdr_netobj	cr_princhash;
};

/* A reasonable value for REPLAY_ISIZE was estimated as follows:  
 * The OPEN response, typically the largest, requires 
 *   4(status) + 8(stateid) + 20(changeinfo) + 4(rflags) +  8(verifier) + 
 *   4(deleg. type) + 8(deleg. stateid) + 4(deleg. recall flag) + 
 *   20(deleg. space limit) + ~32(deleg. ace) = 112 bytes 
 */

#define NFSD4_REPLAY_ISIZE       112 

/*
 * Replay buffer, where the result of the last seqid-mutating operation 
 * is cached. 
 */
struct nfs4_replay {
	__be32			rp_status;
	unsigned int		rp_buflen;
	char			*rp_buf;
	struct knfsd_fh		rp_openfh;
	int			rp_locked;
	char			rp_ibuf[NFSD4_REPLAY_ISIZE];
};

struct nfs4_stateowner;

struct nfs4_stateowner_operations {
	void (*so_unhash)(struct nfs4_stateowner *);
	void (*so_free)(struct nfs4_stateowner *);
};

/*
 * A core object that represents either an open or lock owner. The object and
 * lock owner objects have one of these embedded within them. Refcounts and
 * other fields common to both owner types are contained within these
 * structures.
 */
struct nfs4_stateowner {
	struct list_head			so_strhash;
	struct list_head			so_stateids;
	struct nfs4_client			*so_client;
	const struct nfs4_stateowner_operations	*so_ops;
	/* after increment in nfsd4_bump_seqid, represents the next
	 * sequence id expected from the client: */
	atomic_t				so_count;
	u32					so_seqid;
	struct xdr_netobj			so_owner; /* open owner name */
	struct nfs4_replay			so_replay;
	bool					so_is_open_owner;
};

/*
 * When a file is opened, the client provides an open state owner opaque string
 * that indicates the "owner" of that open. These objects are refcounted.
 * References to it are held by each open state associated with it. This object
 * is a superset of the nfs4_stateowner struct.
 */
struct nfs4_openowner {
	struct nfs4_stateowner	oo_owner; /* must be first field */
	struct list_head        oo_perclient;
	/*
	 * We keep around openowners a little while after last close,
	 * which saves clients from having to confirm, and allows us to
	 * handle close replays if they come soon enough.  The close_lru
	 * is a list of such openowners, to be reaped by the laundromat
	 * thread eventually if they remain unused:
	 */
	struct list_head	oo_close_lru;
	struct nfs4_ol_stateid *oo_last_closed_stid;
	time64_t		oo_time; /* time of placement on so_close_lru */
#define NFS4_OO_CONFIRMED   1
	unsigned char		oo_flags;
};

/*
 * Represents a generic "lockowner". Similar to an openowner. References to it
 * are held by the lock stateids that are created on its behalf. This object is
 * a superset of the nfs4_stateowner struct.
 */
struct nfs4_lockowner {
	struct nfs4_stateowner	lo_owner;	/* must be first element */
	struct list_head	lo_blocked;	/* blocked file_locks */
};

static inline struct nfs4_openowner * openowner(struct nfs4_stateowner *so)
{
	return container_of(so, struct nfs4_openowner, oo_owner);
}

static inline struct nfs4_lockowner * lockowner(struct nfs4_stateowner *so)
{
	return container_of(so, struct nfs4_lockowner, lo_owner);
}

/*
 * Per-client state indicating no. of opens and outstanding delegations
 * on a file from a particular client.'od' stands for 'open & delegation'
 */
struct nfs4_clnt_odstate {
	struct nfs4_client	*co_client;
	struct nfs4_file	*co_file;
	struct list_head	co_perfile;
	refcount_t		co_odcount;
};

/*
 * nfs4_file: a file opened by some number of (open) nfs4_stateowners.
 *
 * These objects are global. nfsd keeps one instance of a nfs4_file per
 * filehandle (though it may keep multiple file descriptors for each). Each
 * inode can have multiple filehandles associated with it, so there is
 * (potentially) a many to one relationship between this struct and struct
 * inode.
 */
struct nfs4_file {
	refcount_t		fi_ref;
	struct inode *		fi_inode;
	bool			fi_aliased;
	spinlock_t		fi_lock;
	struct rhlist_head	fi_rlist;
	struct list_head        fi_stateids;
	union {
		struct list_head	fi_delegations;
		struct rcu_head		fi_rcu;
	};
	struct list_head	fi_clnt_odstate;
	/* One each for O_RDONLY, O_WRONLY, O_RDWR: */
	struct nfsd_file	*fi_fds[3];
	/*
	 * Each open or lock stateid contributes 0-4 to the counts
	 * below depending on which bits are set in st_access_bitmap:
	 *     1 to fi_access[O_RDONLY] if NFS4_SHARE_ACCES_READ is set
	 *   + 1 to fi_access[O_WRONLY] if NFS4_SHARE_ACCESS_WRITE is set
	 *   + 1 to both of the above if NFS4_SHARE_ACCESS_BOTH is set.
	 */
	atomic_t		fi_access[2];
	u32			fi_share_deny;
	struct nfsd_file	*fi_deleg_file;
	struct nfsd_file	*fi_rdeleg_file;
	int			fi_delegees;
	struct knfsd_fh		fi_fhandle;
	bool			fi_had_conflict;
#ifdef CONFIG_NFSD_PNFS
	struct list_head	fi_lo_states;
	atomic_t		fi_lo_recalls;
#endif
};

/*
 * A generic struct representing either a open or lock stateid. The nfs4_client
 * holds a reference to each of these objects, and they in turn hold a
 * reference to their respective stateowners. The client's reference is
 * released in response to a close or unlock (depending on whether it's an open
 * or lock stateid) or when the client is being destroyed.
 *
 * In the case of v4.0 open stateids, these objects are preserved for a little
 * while after close in order to handle CLOSE replays. Those are eventually
 * reclaimed via a LRU scheme by the laundromat.
 *
 * This object is a superset of the nfs4_stid. "ol" stands for "Open or Lock".
 * Better suggestions welcome.
 */
struct nfs4_ol_stateid {
	struct nfs4_stid		st_stid;
	struct list_head		st_perfile;
	struct list_head		st_perstateowner;
	struct list_head		st_locks;
	struct nfs4_stateowner		*st_stateowner;
	struct nfs4_clnt_odstate	*st_clnt_odstate;
/*
 * These bitmasks use 3 separate bits for READ, ALLOW, and BOTH; see the
 * comment above bmap_to_share_mode() for explanation:
 */
	unsigned char			st_access_bmap;
	unsigned char			st_deny_bmap;
	struct nfs4_ol_stateid		*st_openstp;
	struct mutex			st_mutex;
};

static inline struct nfs4_ol_stateid *openlockstateid(struct nfs4_stid *s)
{
	return container_of(s, struct nfs4_ol_stateid, st_stid);
}

struct nfs4_layout_stateid {
	struct nfs4_stid		ls_stid;
	struct list_head		ls_perclnt;
	struct list_head		ls_perfile;
	spinlock_t			ls_lock;
	struct list_head		ls_layouts;
	u32				ls_layout_type;
	struct nfsd_file		*ls_file;
	struct nfsd4_callback		ls_recall;
	stateid_t			ls_recall_sid;
	bool				ls_recalled;
	struct mutex			ls_mutex;
};

static inline struct nfs4_layout_stateid *layoutstateid(struct nfs4_stid *s)
{
	return container_of(s, struct nfs4_layout_stateid, ls_stid);
}

/* flags for preprocess_seqid_op() */
#define RD_STATE	        0x00000010
#define WR_STATE	        0x00000020

enum nfsd4_cb_op {
	NFSPROC4_CLNT_CB_NULL = 0,
	NFSPROC4_CLNT_CB_RECALL,
	NFSPROC4_CLNT_CB_LAYOUT,
	NFSPROC4_CLNT_CB_OFFLOAD,
	NFSPROC4_CLNT_CB_SEQUENCE,
	NFSPROC4_CLNT_CB_NOTIFY_LOCK,
	NFSPROC4_CLNT_CB_RECALL_ANY,
	NFSPROC4_CLNT_CB_GETATTR,
};

/* Returns true iff a is later than b: */
static inline bool nfsd4_stateid_generation_after(stateid_t *a, stateid_t *b)
{
	return (s32)(a->si_generation - b->si_generation) > 0;
}

/*
 * When a client tries to get a lock on a file, we set one of these objects
 * on the blocking lock. When the lock becomes free, we can then issue a
 * CB_NOTIFY_LOCK to the server.
 */
struct nfsd4_blocked_lock {
	struct list_head	nbl_list;
	struct list_head	nbl_lru;
	time64_t		nbl_time;
	struct file_lock	nbl_lock;
	struct knfsd_fh		nbl_fh;
	struct nfsd4_callback	nbl_cb;
	struct kref		nbl_kref;
};

struct nfsd4_compound_state;
struct nfsd_net;
struct nfsd4_copy;

extern __be32 nfs4_preprocess_stateid_op(struct svc_rqst *rqstp,
		struct nfsd4_compound_state *cstate, struct svc_fh *fhp,
		stateid_t *stateid, int flags, struct nfsd_file **filp,
		struct nfs4_stid **cstid);
__be32 nfsd4_lookup_stateid(struct nfsd4_compound_state *cstate,
			    stateid_t *stateid, unsigned short typemask,
			    unsigned short statusmask,
			    struct nfs4_stid **s, struct nfsd_net *nn);
struct nfs4_stid *nfs4_alloc_stid(struct nfs4_client *cl, struct kmem_cache *slab,
				  void (*sc_free)(struct nfs4_stid *));
int nfs4_init_copy_state(struct nfsd_net *nn, struct nfsd4_copy *copy);
void nfs4_free_copy_state(struct nfsd4_copy *copy);
struct nfs4_cpntf_state *nfs4_alloc_init_cpntf_state(struct nfsd_net *nn,
			struct nfs4_stid *p_stid);
void nfs4_put_stid(struct nfs4_stid *s);
void nfs4_inc_and_copy_stateid(stateid_t *dst, struct nfs4_stid *stid);
void nfs4_remove_reclaim_record(struct nfs4_client_reclaim *, struct nfsd_net *);
extern void nfs4_release_reclaim(struct nfsd_net *);
extern struct nfs4_client_reclaim *nfsd4_find_reclaim_client(struct xdr_netobj name,
							struct nfsd_net *nn);
extern __be32 nfs4_check_open_reclaim(struct nfs4_client *);
extern void nfsd4_probe_callback(struct nfs4_client *clp);
extern void nfsd4_probe_callback_sync(struct nfs4_client *clp);
extern void nfsd4_change_callback(struct nfs4_client *clp, struct nfs4_cb_conn *);
extern void nfsd41_cb_referring_call(struct nfsd4_callback *cb,
				     struct nfs4_sessionid *sessionid,
				     u32 slotid, u32 seqno);
extern void nfsd41_cb_destroy_referring_call_list(struct nfsd4_callback *cb);
extern void nfsd4_init_cb(struct nfsd4_callback *cb, struct nfs4_client *clp,
		const struct nfsd4_callback_ops *ops, enum nfsd4_cb_op op);
extern bool nfsd4_run_cb(struct nfsd4_callback *cb);

static inline void nfsd4_try_run_cb(struct nfsd4_callback *cb)
{
	if (!test_and_set_bit(NFSD4_CALLBACK_RUNNING, &cb->cb_flags))
		WARN_ON_ONCE(!nfsd4_run_cb(cb));
}

extern void nfsd4_shutdown_callback(struct nfs4_client *);
extern void nfsd4_shutdown_copy(struct nfs4_client *clp);
void nfsd4_async_copy_reaper(struct nfsd_net *nn);
bool nfsd4_has_active_async_copies(struct nfs4_client *clp);
extern struct nfs4_client_reclaim *nfs4_client_to_reclaim(struct xdr_netobj name,
				struct xdr_netobj princhash, struct nfsd_net *nn);
extern bool nfs4_has_reclaimed_state(struct xdr_netobj name, struct nfsd_net *nn);

void put_nfs4_file(struct nfs4_file *fi);
extern void nfs4_put_cpntf_state(struct nfsd_net *nn,
				 struct nfs4_cpntf_state *cps);
extern __be32 manage_cpntf_state(struct nfsd_net *nn, stateid_t *st,
				 struct nfs4_client *clp,
				 struct nfs4_cpntf_state **cps);
static inline void get_nfs4_file(struct nfs4_file *fi)
{
	refcount_inc(&fi->fi_ref);
}
struct nfsd_file *find_any_file(struct nfs4_file *f);

#ifdef CONFIG_NFSD_V4
void nfsd4_revoke_states(struct net *net, struct super_block *sb);
#else
static inline void nfsd4_revoke_states(struct net *net, struct super_block *sb)
{
}
#endif

/* grace period management */
void nfsd4_end_grace(struct nfsd_net *nn);

/* nfs4recover operations */
extern int nfsd4_client_tracking_init(struct net *net);
extern void nfsd4_client_tracking_exit(struct net *net);
extern void nfsd4_client_record_create(struct nfs4_client *clp);
extern void nfsd4_client_record_remove(struct nfs4_client *clp);
extern int nfsd4_client_record_check(struct nfs4_client *clp);
extern void nfsd4_record_grace_done(struct nfsd_net *nn);

static inline bool try_to_expire_client(struct nfs4_client *clp)
{
	cmpxchg(&clp->cl_state, NFSD4_COURTESY, NFSD4_EXPIRABLE);
	return clp->cl_state == NFSD4_EXPIRABLE;
}

extern __be32 nfsd4_deleg_getattr_conflict(struct svc_rqst *rqstp,
		struct dentry *dentry, struct nfs4_delegation **pdp);
#endif   /* NFSD4_STATE_H */

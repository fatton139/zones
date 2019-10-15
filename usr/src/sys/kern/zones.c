#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/tree.h>
#include <sys/rwlock.h>
#include <sys/mount.h>
#include <sys/atomic.h>
#include <sys/syscallargs.h>
#include <sys/sysctl.h>

#include <sys/_zones.h>
#include <sys/zones.h>

struct zone_entry {
	TAILQ_ENTRY(zone_entry) entry;
	zoneid_t zid;
	char *zname;
};
TAILQ_HEAD(zone_list, zone_entry);

struct zone_list zone_entries = TAILQ_HEAD_INITIALIZER(zone_entries);

struct rwlock zone_lock = RWLOCK_INITIALIZER("zone_lock");

int queue_size = 1;

struct zone_entry *
get_zone_by_name(const char *zonename)
{
	struct zone_entry *zentry;

	rw_enter_read(&zone_lock);
	TAILQ_FOREACH(zentry, &zone_entries, entry) {
		if (strcmp(zonename, zentry->zname) == 0) {
			rw_exit_read(&zone_lock);
			return (zentry);
		}
	}
	rw_exit_read(&zone_lock);
	return (NULL);
}

struct zone_entry *
get_zone_by_id(zoneid_t id)
{
	struct zone_entry *zentry;

	rw_enter_read(&zone_lock);
	TAILQ_FOREACH(zentry, &zone_entries, entry) {
		if (zentry->zid == id) {
			rw_exit_read(&zone_lock);
			return (zentry);
		}
	}
	rw_exit_read(&zone_lock);
	return (NULL);
}

zoneid_t
get_next_available_id(void)
{
	struct zone_entry *zentry;
	int temp, n;
	int *ids;

	// printf("queue-----\n");
	// TAILQ_FOREACH(zentry, &zone_entries, entry) {
	// 	printf("elem: %s %i\n", zentry->zname, zentry->zid);
	// }
	// printf("queue-----\n");

	ids = malloc(sizeof(int) * queue_size, M_PROC, M_WAITOK);

	n = 0;
	rw_enter_read(&zone_lock);
	TAILQ_FOREACH(zentry, &zone_entries, entry) {
		ids[n] = zentry->zid;
		n++;
	}
	rw_exit_read(&zone_lock);

	for (int i = 0; i < n; i++) {
		for (int j = 0; j < n; j++) {
			if (ids[i] < ids[j]) {
				temp = ids[i];
				ids[i] = ids[j];
				ids[j] = temp;
			}
		}
	}

	// for (int i = 0; i < n; i++) {
	// 	printf("sorted ids: %i\n", ids[i]);
	// }

	for (int i = 1; i < n; i++) {
		if (ids[i] - ids[i - 1] != 1) {
			// printf("available id (gap) %i\n", i + 1);
			return (i + 1);
		}
	}
	// printf("available id %i\n", n + 1);
	return (n + 1);

	// index = 1;
	// TAILQ_FOREACH(zentry, &zone_entries, entry) {
	// 	if (zentry->zid != ids[index - 1]) {
	// 		printf("a NEXT AVAILABLE ID AT %i\n", index);
	// 		return (index);
	// 	}
	// 	index++;
	// }
	// printf("b NEXT AVAILABLE ID AT %i\n", index);
	// return index - 1;
}

int
sys_zone_create(struct proc *p, void *v, register_t *retval)
{
    	printf("%s!\n", __func__);
	
	struct sys_zone_create_args /* {
		syscallarg(const char *) zonename;
	} */ *uap = v;

	struct zone_entry *zentry;
	const char *zname;
	// char zname_buf[MAXZONENAMELEN];
	int zname_len;
	size_t done;

	*retval = -1;
	zname = SCARG(uap, zonename);
	zname_len = strlen(zname);
	
	/* ENAMETOOLONG the name of the zone exceeds MAXZONENAMELEN */
	if (zname_len > MAXZONENAMELEN) {
		return (ENAMETOOLONG);
	}

	/* EPERM the current program is not in the global zone */
	/* EPERM the current user is not root */

	/* EEXIST a zone with the specified name already exists */
	if (get_zone_by_name(zname) != NULL) {
		return (EEXIST);
	}

	/* ERANGE too many zones are currently running */
	if (queue_size >= MAXZONES) {
		return (ERANGE);
	}
	/* EFAULT zonename points to a bad address */

	/* EINVAL the name of the zone contains invalid characters */

	zentry = malloc(sizeof(struct zone_entry), M_PROC, M_WAITOK);
	zentry->zid = get_next_available_id();
	zentry->zname =
	    malloc((zname_len + 1) * sizeof(char), M_PROC, M_WAITOK);
	copyinstr(zname, zentry->zname, zname_len + 1, &done);

	printf("zone created: %s %i\n", zentry->zname, zentry->zid);

	rw_enter_write(&zone_lock);
	TAILQ_INSERT_TAIL(&zone_entries, zentry, entry);
	queue_size++;
	rw_exit_write(&zone_lock);

	*retval = zentry->zid;

    	return (0);
}

int
sys_zone_destroy(struct proc *p, void *v, register_t *retval)
{
    	printf("%s!\n", __func__);

	struct sys_zone_destroy_args /* {
		syscallarg(zoneid_t) z;
	} */ *uap = v;

	struct zone_entry *zentry;
	*retval = -1;
	/* EPERM the current program is not in the global zone */
	/* EPERM the current user is not root */
	/* ESRCH the specified zone does not exist */
	if ((zentry = get_zone_by_id(SCARG(uap, z))) == NULL) {
		return (ESRCH);
	}
	/* EBUSY the specified zone is still in use, ie, a process is still running in the zone */

	printf("zone destroyed: %s %i\n", zentry->zname, zentry->zid);

	rw_enter_write(&zone_lock);
	free(zentry->zname, M_PROC, M_WAITOK);
	TAILQ_REMOVE(&zone_entries, zentry, entry);
	queue_size--;
	rw_exit_write(&zone_lock);

	*retval = 0;
    	return (0);
}

int
sys_zone_enter(struct proc *p, void *v, register_t *retval)
{
    	printf("%s!\n", __func__);

	struct sys_zone_destroy_args /* {
		syscallarg(zoneid_t) z;
	} */ *uap = v;

	struct zone_entry *zentry;
	int mib[2];

	/* EPERM the current program is not in the global zone */
	/* EPERM the current user is not root */
	/* ESRCH the specified zone does not exist */
	if ((zentry = get_zone_by_id(SCARG(uap, z))) == NULL) {
		return (ESRCH);
	}
	printf("entering %i\n", zentry->zid);
	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	struct kinfo_proc kinfo, kinfo_updated;
	// memcpy(kinfo_updated, &kinfo, sizeof(kinfo_proc));
	kinfo_updated = kinfo;
	kinfo_updated.p_zoneid = zentry->zid;
	size_t len;
	if (kern_sysctl(mib, 2, &kinfo, &len, &kinfo_updated, sizeof(struct kinfo_proc), p) == -1) {
		printf("sysctl err\n");
	}
	printf("proc: %i %i %i\n", kinfo.p_zoneid, kinfo.p_pid, kinfo.p_uid);
    	return (0);
}

int
sys_zone_list(struct proc *p, void *v, register_t *retval)
{
    	printf("%s!\n", __func__);

	struct sys_zone_list_args /* {
		syscallarg(zoneid_t *) zs;
		syscallarg(size_t *) nzs;
	} */ *uap = v;

	struct zone_entry *zentry;
	zoneid_t *ids;
	int n, nzs_in;

	copyin(SCARG(uap, nzs), &nzs_in, sizeof(size_t *));

	/* EFAULT zs or nzs point to a bad address */
	/* ERANGE if the number at nzs is less than the number of running zones in the system */

	ids = malloc(sizeof(zoneid_t) * queue_size, M_PROC, M_WAITOK);
	n = 0;
	rw_enter_read(&zone_lock);
	TAILQ_FOREACH(zentry, &zone_entries, entry) {
		ids[n] = zentry->zid;
		n++;
	}
	rw_exit_read(&zone_lock);

	int err = copyout(ids, SCARG(uap, zs), sizeof(zoneid_t) * n);
	free(ids, M_TEMP, sizeof(zoneid_t) * n);
	printf("err1: %i\n", err);
	err = copyout(&n, SCARG(uap, nzs), sizeof(size_t *));
	printf("err2: %i\n", err);
    	return (0);
}

int
sys_zone_name(struct proc *p, void *v, register_t *retval)
{
    	printf("%s!\n", __func__);

	struct sys_zone_name_args /* {
		syscallarg(zoneid_t) z;
		syscallarg(char *) name;
		syscallarg(size_t) namelen;
	} */ *uap = v;
	struct zone_entry *zentry;
	zoneid_t zid;

	zid = SCARG(uap, z);

	if (zid == -1) {
		// return current zone id;
		return (0);
	}

	/* ESRCH The specified zone does not exist */
	if ((zentry = get_zone_by_id(zid)) == NULL) {
		return (ESRCH);
	}
	/* ESRCH The specified zone is not visible in a non-global zone */
	/* EFAULT name refers to a bad memory address */
	/* ENAMETOOLONG The requested name is longer than namelen bytes. */

	copyoutstr(zentry->zname, SCARG(uap, name), SCARG(uap, namelen), NULL);

    	return (0);
}

int
sys_zone_lookup(struct proc *p, void *v, register_t *retval)
{
    	printf("%s!\n", __func__);

	struct sys_zone_lookup_args /* {
		syscallarg(char *) name;
	} */ *uap = v;
	struct zone_entry *zentry;
	const char *zname;
	
	zname = SCARG(uap, name);

	if (zname == NULL) {
		*retval = p->p_p->ps_pid;
		return (0);
	}

	/* ESRCH The specified zone does not exist */
	if ((zentry = get_zone_by_name(SCARG(uap, name))) == NULL) {
		return (ESRCH);
	}
	/* ESRCH The specified zone is not visible in a non-global zone */
	/* EFAULT name refers to a bad memory address */
	/* ENAMETOOLONG the name of the zone exceeds MAXZONENAMELEN */

	*retval = zentry->zid;

    	return (0);
}

/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Device nodes of LinuxKPI drivers that are not network drivers.
 *
 * Haiku's freebsd_network publishes only network interfaces
 * (net/<driver>/<n>). Other Linux driver classes (Bluetooth controllers,
 * input, sound, ...) publish their own nodes here; the add-on's
 * publish_devices()/find_device() (lkpi_driver_glue.h) report this list.
 *
 * Changes reach devfs through devfs_rescan_driver(), the private kernel
 * call Haiku's USB stack uses to make devfs ask a legacy driver for its
 * nodes again [V: Stack::RescanDrivers]. It takes devfs's driver lock, so
 * it is NOT called:
 *  - while init_driver()/uninit_driver() run (devfs holds that lock and
 *    asks for the list itself right after init; a rescan would re-enter
 *    load_driver);
 *  - from a bus notification callback (lkpi_devfs_notify_enter/exit):
 *    Haiku's USB stack calls those holding its explore lock and rescans
 *    the driver itself after releasing it; rescanning from inside would
 *    invert the lock order against a driver load that waits for the
 *    explore lock in install_notify() [V: Stack::Explore unlocks before
 *    RescanDrivers for exactly this reason].
 *
 * Built WITHOUT the LinuxKPI preinclude (Haiku side).
 */
#include <KernelExport.h>
#include <Drivers.h>
#include <lock.h>
#include <fs/devfs.h>
#include <stdlib.h>
#include <string.h>

#include "lkpi_driver.h"

extern const char *gDriverName;		/* freebsd_network haiku-module.h */

struct lkpi_devfs_node {
	struct lkpi_devfs_node	*next;
	device_hooks		*hooks;
	char			name[];
};

static mutex sLock = MUTEX_INITIALIZER("lkpi devfs");
static struct lkpi_devfs_node *sNodes;
static int sCount;
static char **sPublished;	/* last list handed to devfs */
static bool sQuiet;		/* inside init_driver()/uninit_driver() */
static thread_id sNotifyThread = -1;	/* in a bus notification callback */

static void
lkpi_devfs_changed(void)
{
	bool rescan;

	mutex_lock(&sLock);
	rescan = !sQuiet && sNotifyThread != find_thread(NULL);
	mutex_unlock(&sLock);
	if (rescan)
		devfs_rescan_driver(gDriverName);
}

/* Bus layers bracket their device added/removed callbacks with these;
 * the bus itself makes devfs rescan afterwards. Not nested. */
void
lkpi_devfs_notify_enter(void)
{
	mutex_lock(&sLock);
	sNotifyThread = find_thread(NULL);
	mutex_unlock(&sLock);
}

void
lkpi_devfs_notify_exit(void)
{
	mutex_lock(&sLock);
	sNotifyThread = -1;
	mutex_unlock(&sLock);
}

status_t
lkpi_devfs_add(const char *name, device_hooks *hooks)
{
	struct lkpi_devfs_node *n, *it;
	size_t len;

	if (name == NULL || hooks == NULL)
		return (B_BAD_VALUE);
	len = strlen(name);
	if (len == 0 || len >= B_PATH_NAME_LENGTH)
		return (B_BAD_VALUE);
	n = malloc(sizeof(*n) + len + 1);
	if (n == NULL)
		return (B_NO_MEMORY);
	n->hooks = hooks;
	memcpy(n->name, name, len + 1);

	mutex_lock(&sLock);
	for (it = sNodes; it != NULL; it = it->next) {
		if (strcmp(it->name, name) == 0) {
			mutex_unlock(&sLock);
			free(n);
			return (B_NAME_IN_USE);
		}
	}
	n->next = sNodes;
	sNodes = n;
	sCount++;
	mutex_unlock(&sLock);

	lkpi_devfs_changed();
	return (B_OK);
}

void
lkpi_devfs_remove(const char *name)
{
	struct lkpi_devfs_node **pp, *n = NULL;

	mutex_lock(&sLock);
	for (pp = &sNodes; *pp != NULL; pp = &(*pp)->next) {
		if (strcmp((*pp)->name, name) == 0) {
			n = *pp;
			*pp = n->next;
			sCount--;
			break;
		}
	}
	mutex_unlock(&sLock);
	if (n == NULL)
		return;
	free(n);
	/* devfs unpublishes it; open handles keep their hooks and cookies,
	 * so the owner must keep those valid until free() */
	lkpi_devfs_changed();
}

int
lkpi_devfs_count(void)
{
	int n;

	mutex_lock(&sLock);
	n = sCount;
	mutex_unlock(&sLock);
	return (n);
}

/* publish_devices(): a NULL-terminated snapshot, valid until next call */
const char **
lkpi_devfs_publish(void)
{
	struct lkpi_devfs_node *n;
	char **list;
	int i = 0;

	mutex_lock(&sLock);
	list = calloc(sCount + 1, sizeof(char *));
	if (list != NULL) {
		for (n = sNodes; n != NULL; n = n->next) {
			list[i] = strdup(n->name);
			if (list[i] != NULL)
				i++;
		}
	}
	if (sPublished != NULL) {
		for (i = 0; sPublished[i] != NULL; i++)
			free(sPublished[i]);
		free(sPublished);
	}
	sPublished = list;
	mutex_unlock(&sLock);
	return ((const char **)list);
}

device_hooks *
lkpi_devfs_find(const char *name)
{
	struct lkpi_devfs_node *n;
	device_hooks *hooks = NULL;

	mutex_lock(&sLock);
	for (n = sNodes; n != NULL; n = n->next) {
		if (strcmp(n->name, name) == 0) {
			hooks = n->hooks;
			break;
		}
	}
	mutex_unlock(&sLock);
	return (hooks);
}

void
lkpi_devfs_set_quiet(bool quiet)
{
	mutex_lock(&sLock);
	sQuiet = quiet;
	mutex_unlock(&sLock);
}

/* uninit_driver(): drop everything (devfs already stopped asking) */
void
lkpi_devfs_cleanup(void)
{
	struct lkpi_devfs_node *n;
	int i;

	mutex_lock(&sLock);
	while ((n = sNodes) != NULL) {
		sNodes = n->next;
		free(n);
	}
	sCount = 0;
	if (sPublished != NULL) {
		for (i = 0; sPublished[i] != NULL; i++)
			free(sPublished[i]);
		free(sPublished);
		sPublished = NULL;
	}
	mutex_unlock(&sLock);
}

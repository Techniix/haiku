/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay: extends LinuxKPI's linux/skbuff.h (FreeBSD import) with
 * sk_buff helpers it lacks and Bluetooth drivers use. If a later FreeBSD
 * import adds one of these, the build fails on the duplicate definition:
 * delete it here then.
 */
#ifndef _LKPI_HAIKU_LINUX_SKBUFF_H_
#define _LKPI_HAIKU_LINUX_SKBUFF_H_

#include_next <linux/skbuff.h>

/* Linux 5.17: pull len bytes, return a pointer to them, NULL if short. */
static inline void *
skb_pull_data(struct sk_buff *skb, size_t len)
{
	void *data = skb->data;

	if (skb->len < len)
		return (NULL);
	skb_pull(skb, len);
	return (data);
}

/*
 * Linux shares the data between clone and original. A full copy has the
 * same contents, header fields and cb[] (linuxkpi_skb_copy()), and differs
 * only where a caller writes the data through one and expects to see it
 * through the other; drivers using clones to parse (btusb, btmtk) do not.
 */
static inline struct sk_buff *
skb_clone(struct sk_buff *skb, gfp_t gfp)
{
	return (skb_copy(skb, gfp));
}

/*
 * Linux: pad with zeros up to len bytes. Only within the tailroom here
 * (LinuxKPI's skb_pad() cannot grow a buffer yet); on failure the skb is
 * freed if free_on_error, as in Linux.
 */
static inline int
__skb_put_padto(struct sk_buff *skb, unsigned int len, bool free_on_error)
{
	if (skb->len >= len)
		return (0);
	if (skb_tailroom(skb) < (int)(len - skb->len)) {
		if (free_on_error)
			kfree_skb(skb);
		return (-ENOMEM);
	}
	skb_put_zero(skb, len - skb->len);
	return (0);
}

static inline int
skb_put_padto(struct sk_buff *skb, unsigned int len)
{
	return (__skb_put_padto(skb, len, true));
}

#endif

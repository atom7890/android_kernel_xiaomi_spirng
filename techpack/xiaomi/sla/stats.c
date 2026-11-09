/*
    Copyright (c) 2018, The Linux Foundation. All rights reserved.

    This program is free software; you can redistribute it and/or modify it under the terms
    of the GNU General Public License version 2 and only version 2 as published by the Free
    Software Foundation.

    This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
    without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
    See the GNU General Public License for more details.
*/
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/module.h>
#include <asm/uaccess.h>
#include <linux/seq_file.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/ip.h>
#include <net/ip.h>
#include <net/sock.h>
#include <linux/version.h>
#include <linux/skbuff.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/moduleparam.h>
#include <linux/spinlock.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/hash.h>
#include <linux/rculist.h>
#include <linux/debugfs.h>
#include <linux/proc_fs.h>
#include <linux/list.h>
#include <linux/ktime.h>

#include <linux/ctype.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/vmalloc.h>
#include "sla_stats.h"

static spinlock_t stats_spin_lock;
static DEFINE_MUTEX(config_mutex);

/*
 * One owner reference while the interface is registered, plus references
 * held by statistics files, counter files and VMAs.
 */
static void sla_iface_put(struct sla_interface *iface)
{
    if (refcount_dec_and_test(&iface->refs)) {
        vfree(iface->mmap_struct);
        kfree(iface);
    }
}

static int sla_iface_open_get(struct sla_interface *iface)
{
    if (!iface || !refcount_inc_not_zero(&iface->refs))
        return -ENODEV;
    if (!try_module_get(THIS_MODULE)) {
        sla_iface_put(iface);
        return -ENODEV;
    }
    return 0;
}

static void sla_vma_open(struct vm_area_struct *vma)
{
    struct sla_interface *iface = vma->vm_private_data;

    /* The existing mapping already pins both module and interface. */
    __module_get(THIS_MODULE);
    refcount_inc(&iface->refs);
}

static void sla_vma_close(struct vm_area_struct *vma)
{
    sla_iface_put(vma->vm_private_data);
    module_put(THIS_MODULE);
}

static const struct vm_operations_struct sla_vm_ops = {
    .open = sla_vma_open,
    .close = sla_vma_close,
};

static int sla_stats_release(struct inode *inode, struct file *file)
{
    sla_iface_put(file->private_data);
    module_put(THIS_MODULE);
    return 0;
}

static int sla_counter_release(struct inode *inode, struct file *file)
{
    struct sla_interface *iface = pde_data(inode);
    int ret = simple_attr_release(inode, file);

    sla_iface_put(iface);
    module_put(THIS_MODULE);
    return ret;
}

/* about fs config */
#define USE_PROCFS

/* about stats */
#define STATS_BUF_SIZE (sizeof(struct sla_interface_stats)) //16*(1+ENTRY_TOTAL) = (20*1024)

#define MMAP_FILE_SUFFIX "_mmap"
#define TOTAL_STATS_FILE_SUFFIX "_stats"
#define VOIP_STATS_FILE "sla_voip_state"

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 17, 0))
/*
 * Commit 359745d78351 ("proc: remove PDE_DATA() completely")
 * Replaced PDE_DATA() with pde_data()
 */
#define pde_data(inode) PDE_DATA(inode)
#endif

static struct sla_config config_data;
static struct sla_voip_state voip_state;
static u64 pkt_alloc_num = 0;
static u64 pkt_free_num = 0;


LIST_HEAD(sla_iface_list) ;


/* about port hash list */
#define PORT_HASH_BITS 8
#define PORT_HASH_SIZE (1<<PORT_HASH_BITS)
#define MAX_SIZE_DEFAULT    (4*1024)
#define LIMIT_TIME_DEFAULT  (5)

#define STATS_LOCK_IRQ spin_lock_irqsave
#define STATS_UNLOCK_IRQ spin_unlock_irqrestore

struct uplink_pkt
{
    struct hlist_node   hlist;
    u16 local_port;
};

struct hlist_head   pkt_list[PORT_HASH_SIZE];


/* about fs */

static int procfs_u64_set(void *data, u64 val)
{
    struct sla_interface *iface = data;
    unsigned long flags;

    spin_lock_irqsave(&stats_spin_lock, flags);
    iface->total_trfc = val;
    spin_unlock_irqrestore(&stats_spin_lock, flags);
    return 0;
}


static int procfs_u64_get(void *data, u64 *val)
{
    struct sla_interface *iface = data;
    unsigned long flags;

    spin_lock_irqsave(&stats_spin_lock, flags);
    *val = iface->total_trfc;
    spin_unlock_irqrestore(&stats_spin_lock, flags);
    return 0;
}

static int fops_u64_open(struct inode *inode, struct file *filp)
{
    struct sla_interface *iface = pde_data(inode);
    int ret = sla_iface_open_get(iface);

    if (ret)
        return ret;
    inode->i_private = iface;
    ret = simple_attr_open(inode, filp, procfs_u64_get,
                           procfs_u64_set, "%llu\n");
    if (ret) {
        sla_iface_put(iface);
        module_put(THIS_MODULE);
    }
    return ret;
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
static const struct proc_ops fops_u64 =
{
    .proc_open    = fops_u64_open,
    .proc_release = sla_counter_release,
    .proc_read    = simple_attr_read,
    .proc_write   = simple_attr_write,
    .proc_lseek  = generic_file_llseek,
};
#else
static const struct file_operations fops_u64 =
{
    .owner   = THIS_MODULE,
    .open    = fops_u64_open,
    .release = sla_counter_release,
    .read    = simple_attr_read,
    .write   = simple_attr_write,
    .llseek  = generic_file_llseek,
};
#endif


/* dir */
static struct proc_dir_entry *sla_dir;
/* config file */
static struct proc_dir_entry *config_file; //Used for SLA config: enable, prots, reset_traffic and so on

static struct sla_interface * sla_find_interface(const char *interface_name);
/* sla voip file */
static struct proc_dir_entry *sla_voip_file;

/* about lock */


static inline void stats_lock_init(void)
{
    spin_lock_init(&stats_spin_lock);
}

static inline void stats_lock(void)
{
    spin_lock(&stats_spin_lock);
}

static inline void stats_unlock(void)
{
    spin_unlock(&stats_spin_lock);
}

static spinlock_t pkt_spin_lock;

static inline void pkt_lock_init(void)
{
    spin_lock_init(&pkt_spin_lock);
}

static spinlock_t list_spin_lock;

static inline void list_lock_init(void)
{
    spin_lock_init(&list_spin_lock);
}

static inline struct hlist_head *pkt_hlist_head(u16 port)
{
    return &pkt_list[hash_32(port, PORT_HASH_BITS)];
}

static struct uplink_pkt *pkt_find(u16 port)
{
    struct uplink_pkt *pkt;

    hlist_for_each_entry(pkt, pkt_hlist_head(port), hlist)
    {
        if (pkt->local_port == port)
            return pkt;
    }

    return NULL;
}

static void pkt_add(u16 port)
{
    struct uplink_pkt *pkt;

    if(pkt_find(port))
        return;

    pkt = kzalloc(sizeof(*pkt), GFP_ATOMIC);
    if (!pkt)
        return;

    pkt->local_port = port;
    spin_lock(&list_spin_lock);
    ++ pkt_alloc_num;
    hlist_add_head(&pkt->hlist, pkt_hlist_head(port));
    spin_unlock(&list_spin_lock);
}

static void pkt_release(struct uplink_pkt *pkt)
{
    spin_lock(&list_spin_lock);
    ++ pkt_free_num;
    hlist_del(&pkt->hlist);
    spin_unlock(&list_spin_lock);
    kfree(pkt);
}


static void pkt_hlist_init(void)
{
    unsigned int h;

    for (h = 0; h < PORT_HASH_SIZE; h++)
        INIT_HLIST_HEAD(&pkt_list[h]);
}

static void pkt_hlist_deinit(void)
{
    struct uplink_pkt *pkt;
    struct hlist_node *tmp;
    unsigned int h;

    for (h = 0; h < PORT_HASH_SIZE; h++)
        hlist_for_each_entry_safe(pkt, tmp, &pkt_list[h], hlist)
            pkt_release(pkt);
}


static void pkt_hlist_print(void)
{
    struct uplink_pkt *pkt;
    unsigned int h;

    for (h = 0; h < PORT_HASH_SIZE; h++)
    {
        hlist_for_each_entry(pkt, &pkt_list[h], hlist)
            printk("%s() pkt_list[%d] local_port=%d\n", __func__, h, pkt->local_port);
    }
}

static bool sla_pkt_valid(struct sk_buff *skb, u8 link_type)
{
    // int i;
    u16 local_port;
    u16 len;
    struct tcphdr *tcph;
    struct uplink_pkt *pkt;
    unsigned long flags;

    if(IPPROTO_TCP != ip_hdr(skb)->protocol)
        return false;

    if(LT_UPLINK == link_type)
    {
        local_port = ntohs(tcp_hdr(skb)->source);
    }
    else
    {
        local_port = ntohs(tcp_hdr(skb)->dest);
    }

    // for(i=0; (i<PORTS_MAX) && (0!=config_data.ports[i]); i++)
    // {
    //     if(ext_port == config_data.ports[i])
    //         break;
    // }

    // if((i == PORTS_MAX) || ext_port != config_data.ports[i])
    // {
    //     return false;
    // }

    len = ntohs(ip_hdr(skb)->tot_len) - (ip_hdr(skb)->ihl*4 + tcp_hdrlen(skb));
    if(0 == len)
    {
        tcph = tcp_hdr(skb);
        if(tcph->fin || tcph->rst)
        {
            spin_lock_irqsave(&pkt_spin_lock, flags);
            pkt = pkt_find(local_port);
            if(pkt)
            {
                pkt_release(pkt);
            }
            spin_unlock_irqrestore(&pkt_spin_lock, flags);
        }
        return false;
    }

    return true;
}

static bool sla_pkt_consolidate(struct sla_interface_stats *stats, u32 idx)
{
    u32 prev;
    struct sla_stats_entry* entry = &stats->entries[idx];

    if(0 == idx)
        prev = ENTRY_TOTAL - 1;
    else
        prev = idx - 1;

    if(stats->entries[prev].discard)
        return false;

    if(stats->entries[prev].port != entry->port)
        return false;

    if((stats->entries[prev].timestamp + config_data.limit_time) < entry->timestamp)
        return false;

    if((stats->entries[prev].size + entry->size) > config_data.max_size)
        return false;

    stats->entries[prev].size += entry->size;

    return true;
}

static inline bool sla_voip_pack_filter(struct sk_buff *skb)
{
    if (ip_hdr(skb)->protocol == IPPROTO_UDP && !IS_ERR_OR_NULL(skb->sk)) {
        uid_t voip_uid = __kuid_val(skb->sk->sk_uid);
        if (config_data.voip_uid == voip_uid && voip_state.address == 0) {
            __be16 port = udp_hdr(skb)->dest;
            // Exclude port 53 of udp
            if (port != 13568) {
                voip_state.address = ip_hdr(skb)->daddr;
                voip_state.port = port;
            }
        }
            return true;
    }
    return false;
}

static unsigned int sla_ipv4_in_locked(void *priv,
                                struct sk_buff *skb,
                                const struct nf_hook_state *state)
{
    struct sla_interface_stats *p;
    struct sla_stats_entry *entry;
    struct timespec64 tm;
    struct uplink_pkt *pkt;
    bool consolidated = false;
    struct sla_interface* iface = NULL;

    unsigned long pkt_flags;

    if(config_data.enable && config_data.rate_on)
    {
        if(!sla_pkt_valid(skb, LT_DOWNLINK))
            return NF_ACCEPT;

        //printk("#### SLA sla_ipv4_in ");
        //printk("#### %s devname=[%s] mac len=%d skb->len=%d ifaceid = ((%d)) \n", __func__, state->in->name, skb->mac_len, skb->len, state->in->ifindex);

        iface =  sla_find_interface(state->in->name);

        if(iface == NULL)
        {

            return NF_ACCEPT;
        }
        p = iface->mmap_struct;

        iface->total_trfc += skb->len;

        ktime_get_real_ts64(&tm);
        entry = &p->entries[iface->kernel_cur_idx];
        entry->link_type = LT_DOWNLINK;
        entry->timestamp = tm.tv_sec*1000 + tm.tv_nsec/1000000;
        entry->size = skb->len + 14; //14 is mac len
        entry->port = ntohs(tcp_hdr(skb)->dest);

        spin_lock_irqsave(&pkt_spin_lock, pkt_flags);
        pkt = pkt_find(entry->port);
        if(pkt)
        {
            pkt_release(pkt);

            entry->discard = 1;
        }
        else
        {
            entry->discard = 0;
            consolidated = sla_pkt_consolidate(p, iface->kernel_cur_idx);
        }
        spin_unlock_irqrestore(&pkt_spin_lock, pkt_flags);

        if(!consolidated)
        {
            iface->kernel_cur_idx++;
            if(iface->kernel_cur_idx >= ENTRY_TOTAL)
                iface->kernel_cur_idx = 0;
        }
        else
            memset(entry, 0, sizeof(*entry));

        WRITE_ONCE(p->cur_idx, iface->kernel_cur_idx);

    }

    return NF_ACCEPT;
}

static unsigned int sla_ipv4_out_locked(void *priv,
                                 struct sk_buff *skb,
                                 const struct nf_hook_state *state)
{
    struct sla_interface_stats *p;
    struct sla_stats_entry *entry;
    struct timespec64 tm;
    struct sla_interface* iface = NULL;

    if (config_data.enable && config_data.voip_enable && config_data.voip_uid != 0)
    {
        if (sla_voip_pack_filter(skb))
            return NF_ACCEPT;
    }

    if(config_data.enable && config_data.rate_on)
    {
        if(!sla_pkt_valid(skb, LT_UPLINK))
            return NF_ACCEPT;

        //printk("#### SLA sla_ipv4_out ");
        //printk("#### %s devname=[%s] mac-len=%d skb->len=%d ifaceid = ((%d)) \n", __func__, state->out->name, skb->mac_len, skb->len, state->out->ifindex);

        iface =  sla_find_interface(state->out->name);

        if(iface == NULL)
        {

            return NF_ACCEPT;
        }
        p = iface->mmap_struct;

        ktime_get_real_ts64(&tm);
        pkt_add(ntohs(tcp_hdr(skb)->source));

        entry = &p->entries[iface->kernel_cur_idx];
        entry->link_type = LT_UPLINK;

        entry->timestamp = tm.tv_sec*1000 + tm.tv_nsec/1000000;
        entry->size = skb->len + 14; //14 is mac len
        entry->port = ntohs(tcp_hdr(skb)->source);
        entry->discard = 1;

        iface->kernel_cur_idx++;
        if(iface->kernel_cur_idx >= ENTRY_TOTAL)
            iface->kernel_cur_idx = 0;

        WRITE_ONCE(p->cur_idx, iface->kernel_cur_idx);

    }

    return NF_ACCEPT;
}


static bool sla_headers_valid(struct sk_buff *skb)
{
    struct iphdr *iph;
    unsigned int ihl, total, transport;
    int offset;

    if (!skb)
        return false;
    offset = skb_network_offset(skb);
    if (offset < 0 || !pskb_may_pull(skb, offset + sizeof(*iph)))
        return false;

    iph = ip_hdr(skb);
    if (iph->version != 4 || iph->ihl < 5 || ip_is_fragment(iph))
        return false;
    ihl = iph->ihl * 4;
    total = ntohs(iph->tot_len);
    if (total < ihl || total > skb->len - offset)
        return false;

    if (iph->protocol == IPPROTO_TCP)
        transport = sizeof(struct tcphdr);
    else if (iph->protocol == IPPROTO_UDP)
        transport = sizeof(struct udphdr);
    else
        return false;

    if (total < ihl + transport ||
        !pskb_may_pull(skb, offset + ihl + transport))
        return false;

    skb_set_transport_header(skb, offset + ihl);
    iph = ip_hdr(skb);
    if (iph->protocol == IPPROTO_TCP) {
        transport = tcp_hdr(skb)->doff * 4;
        if (transport < sizeof(struct tcphdr) ||
            total < ihl + transport ||
            !pskb_may_pull(skb, offset + ihl + transport))
            return false;
    } else {
        unsigned int len = ntohs(udp_hdr(skb)->len);

        if (len < sizeof(struct udphdr) || len > total - ihl)
            return false;
    }
    return true;
}

static unsigned int sla_ipv4_in(void *priv, struct sk_buff *skb,
                                    const struct nf_hook_state *state)
{
    unsigned long flags;
    unsigned int ret;

    if (!READ_ONCE(config_data.enable) ||
        !READ_ONCE(config_data.rate_on))
        return NF_ACCEPT;
    if (!state->in || !sla_headers_valid(skb))
        return NF_ACCEPT;
    spin_lock_irqsave(&stats_spin_lock, flags);
    ret = sla_ipv4_in_locked(priv, skb, state);
    spin_unlock_irqrestore(&stats_spin_lock, flags);
    return ret;
}

static unsigned int sla_ipv4_out(void *priv, struct sk_buff *skb,
                                    const struct nf_hook_state *state)
{
    unsigned long flags;
    unsigned int ret;

    if (!READ_ONCE(config_data.enable) ||
        (!READ_ONCE(config_data.rate_on) &&
         !READ_ONCE(config_data.voip_enable)))
        return NF_ACCEPT;
    if (!state->out || !sla_headers_valid(skb))
        return NF_ACCEPT;
    spin_lock_irqsave(&stats_spin_lock, flags);
    ret = sla_ipv4_out_locked(priv, skb, state);
    spin_unlock_irqrestore(&stats_spin_lock, flags);
    return ret;
}

static struct nf_hook_ops sla_ipv4_ops[] __read_mostly =
{
    {
        .hook       = sla_ipv4_in,
        .pf         = NFPROTO_IPV4,
        .hooknum    = NF_INET_PRE_ROUTING,
        .priority   = NF_IP_PRI_CONNTRACK+2,
    },
    {
        .hook       = sla_ipv4_out,
        .pf         = NFPROTO_IPV4,
        .hooknum    = NF_INET_POST_ROUTING,
        .priority   = NF_IP_PRI_LAST,
    },
};

static ssize_t stats_read(struct file *file, char __user *user_buf,
                          size_t count, loff_t *ppos)
{
    struct sla_interface *iface = file->private_data;

    return simple_read_from_buffer(user_buf, count, ppos,
                                   iface->mmap_struct, STATS_BUF_SIZE);
}

static int stats_map(struct file *filp, struct vm_area_struct *vma)
{
    struct sla_interface *iface = filp->private_data;
    unsigned long size = vma->vm_end - vma->vm_start;
    int ret;

    if (!iface)
        return -ENODEV;
    if (vma->vm_pgoff || !size || size > PAGE_ALIGN(STATS_BUF_SIZE))
        return -EINVAL;
    if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC))
        return -EINVAL;

    ret = sla_iface_open_get(iface);
    if (ret)
        return ret;

    vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
    vm_flags_clear(vma, VM_MAYEXEC);
    ret = remap_vmalloc_range(vma, iface->mmap_struct, 0);
    if (ret) {
        sla_iface_put(iface);
        module_put(THIS_MODULE);
        return ret;
    }

    vma->vm_private_data = iface;
    vma->vm_ops = &sla_vm_ops;
    return 0;
}


int stats_open(struct inode *inode, struct file *file)
{
    struct sla_interface *iface = pde_data(inode);
    int ret = sla_iface_open_get(iface);

    if (ret)
        return ret;
    file->private_data = iface;
    return 0;
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
static const struct proc_ops stats_fops =
{
    .proc_open = stats_open,
    .proc_release = sla_stats_release,
    .proc_read = stats_read,
    .proc_lseek = default_llseek,
    .proc_mmap = stats_map,
};
#else
static const struct file_operations stats_fops =
{
    .owner = THIS_MODULE,
    .open = stats_open,
    .release = sla_stats_release,
    .read = stats_read,
    .llseek = default_llseek,
    .mmap = stats_map,
};
#endif

/*sla voip state file operations start*/

struct sla_voip_snapshot {
    struct mutex lock;
    bool captured;
    size_t length;
    char text[32];
};

static int sla_voip_file_open(struct inode *inode, struct file *filp)
{
    struct sla_voip_snapshot *snapshot;

    if (!try_module_get(THIS_MODULE))
        return -ENODEV;
    snapshot = kzalloc(sizeof(*snapshot), GFP_KERNEL);
    if (!snapshot) {
        module_put(THIS_MODULE);
        return -ENOMEM;
    }
    mutex_init(&snapshot->lock);
    filp->private_data = snapshot;
    return 0;
}

static int sla_voip_file_release(struct inode *inode, struct file *filp)
{
    kfree(filp->private_data);
    module_put(THIS_MODULE);
    return 0;
}

static ssize_t sla_voip_file_read(struct file *filp, char __user *buffer,
                                  size_t size, loff_t *poss)
{
    struct sla_voip_snapshot *snapshot = filp->private_data;
    struct sla_voip_state state;
    unsigned long flags;
    ssize_t ret;

    if (!size)
        return 0;
    mutex_lock(&snapshot->lock);
    if (!snapshot->captured) {
        spin_lock_irqsave(&stats_spin_lock, flags);
        state = voip_state;
        memset(&voip_state, 0, sizeof(voip_state));
        spin_unlock_irqrestore(&stats_spin_lock, flags);

        snapshot->length = scnprintf(snapshot->text,
            sizeof(snapshot->text), "%u,%d", state.address, state.port);
        snapshot->captured = true;
    }
    ret = simple_read_from_buffer(buffer, size, poss,
                                   snapshot->text, snapshot->length);
    mutex_unlock(&snapshot->lock);
    return ret;
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
static const struct proc_ops sla_voip_fops =
{
    .proc_open = sla_voip_file_open,
    .proc_release = sla_voip_file_release,
    .proc_read = sla_voip_file_read,
};
#else
static const struct file_operations sla_voip_fops =
{
    .owner = THIS_MODULE,
    .open = sla_voip_file_open,
    .release = sla_voip_file_release,
    .read = sla_voip_file_read,
};
#endif
/*sla voip state file operations end*/

static void sla_stats_data_init(struct sla_interface_stats *p)
{
    memset(p, 0, STATS_BUF_SIZE);
    p->magic_num = STATS_MAGIC;
    p->total = ENTRY_TOTAL;
    p->cur_idx = 0;
    p->reserve = 0;
}


/**
 *
 * @param interface_name
 *
 * @return struct sla_interface*
 *
 * This function itterates over sla_iface_list to find the
 * interface structure that matches interface_name.
 *
 * The caller needs to acquire stats_lock() before calling the
 * function and while using the returned sla_interface ptr.
 *
 * The caller also needs to release the lock by calling
 * stats_unlock() after using ( reading or writing to)
 * sla_interface ptr.
 */
static struct sla_interface * sla_find_interface(const char *interface_name)
{
    struct sla_interface * iface = NULL;

    list_for_each_entry (iface , &sla_iface_list, interface_list_hook)
    {
         if(strcmp(interface_name,iface->interface_name ) == 0)
         {
             return iface;
         }
    }

    return NULL;
}

static int sla_interface_start(const char *interface_name)
{
    unsigned long flags;
    char mmap_name[IF_NAME_LEN + 5];
    char counter_name[IF_NAME_LEN + 6];
    struct sla_interface *iface;
    int ret = -ENOMEM;

    if (!interface_name || !interface_name[0] ||
        strnlen(interface_name, IF_NAME_LEN) >= IF_NAME_LEN ||
        strchr(interface_name, '/'))
        return -EINVAL;

    spin_lock_irqsave(&stats_spin_lock, flags);
    iface = sla_find_interface(interface_name);
    spin_unlock_irqrestore(&stats_spin_lock, flags);
    if (iface)
        return -EEXIST;

    iface = kzalloc(sizeof(*iface), GFP_KERNEL);
    if (!iface)
        return -ENOMEM;

    refcount_set(&iface->refs, 1);
    INIT_LIST_HEAD(&iface->interface_list_hook);
    strscpy(iface->interface_name, interface_name,
            sizeof(iface->interface_name));

    iface->mmap_struct = vmalloc_user(PAGE_ALIGN(STATS_BUF_SIZE));
    if (!iface->mmap_struct)
        goto put;

    sla_stats_data_init(iface->mmap_struct);
    snprintf(mmap_name, sizeof(mmap_name), "%s%s",
             interface_name, MMAP_FILE_SUFFIX);
    snprintf(counter_name, sizeof(counter_name), "%s%s",
             interface_name, TOTAL_STATS_FILE_SUFFIX);

    iface->mmap_file = proc_create_data(
        mmap_name, 0644, sla_dir, &stats_fops, iface);
    if (!iface->mmap_file)
        goto put;

    iface->total_stats_file = proc_create_data(
        counter_name, 0644, sla_dir, &fops_u64, iface);
    if (!iface->total_stats_file) {
        proc_remove(iface->mmap_file);
        goto put;
    }

    spin_lock_irqsave(&stats_spin_lock, flags);
    list_add(&iface->interface_list_hook, &sla_iface_list);
    spin_unlock_irqrestore(&stats_spin_lock, flags);
    return 0;

put:
    sla_iface_put(iface);
    return ret;
}

static void sla_stats_disable(void)
{
    struct sla_interface *iface, *next;
    unsigned long flags;
    LIST_HEAD(detached);

    spin_lock_irqsave(&stats_spin_lock, flags);
    config_data.enable = 0;
    config_data.rate_on = 0;
    list_splice_init(&sla_iface_list, &detached);
    spin_unlock_irqrestore(&stats_spin_lock, flags);

    /*
     * proc_remove may wait for active callbacks. Never call it while
     * holding the packet-accounting spinlock.
     */
    list_for_each_entry_safe(iface, next, &detached, interface_list_hook) {
        proc_remove(iface->mmap_file);
        proc_remove(iface->total_stats_file);
        list_del_init(&iface->interface_list_hook);
        sla_iface_put(iface);
    }

    spin_lock_irqsave(&stats_spin_lock, flags);
    spin_lock(&pkt_spin_lock);
    pkt_hlist_deinit();
    spin_unlock(&pkt_spin_lock);
    spin_unlock_irqrestore(&stats_spin_lock, flags);
}

static void sla_stats_enable(void)
{
    unsigned long flags;

    spin_lock_irqsave(&stats_spin_lock, flags);
    if (!config_data.enable) {
        config_data.enable = 1;
        config_data.rate_on = 1;
    }
    spin_unlock_irqrestore(&stats_spin_lock, flags);
}


static int config_file_show(struct seq_file *s, void *data)
{
    struct sla_config snapshot;
    struct sla_config *config = &snapshot;
    unsigned long flags;
    int i;

    spin_lock_irqsave(&stats_spin_lock, flags);
    snapshot = config_data;
    spin_unlock_irqrestore(&stats_spin_lock, flags);

    seq_printf(s, CMD_ENABLE"%d\n", config->enable);
    seq_printf(s, CMD_RATE_ON"%d\n", config->rate_on);

    seq_puts(s, CMD_PORTS);
    if(0 != config->ports[0])
    {
        seq_printf(s, "%d", config->ports[0]);

        for(i=1; (i<PORTS_MAX) && (0!=config->ports[i]); i++)
            seq_printf(s, ",%d", config->ports[i]);
    }

    seq_printf(s, "\n");
    seq_printf(s, CMD_MAX_SIZE"%d\n", config->max_size);
    seq_printf(s, CMD_LIMIT_TIME"%d\n", config->limit_time);
    seq_printf(s, CMD_VOIP_ENABLE"%d\n", config->voip_enable);
    seq_printf(s, CMD_VOIP_UID"%u\n", config->voip_uid);

    return 0;
}

static int config_file_open(struct inode *inode, struct file *filp)
{
    return single_open(filp, config_file_show, pde_data(inode));
}

#define SLA_CONFIG_SET(member, value) do { \
    unsigned long config_flags; \
    spin_lock_irqsave(&stats_spin_lock, config_flags); \
    config_data.member = (value); \
    spin_unlock_irqrestore(&stats_spin_lock, config_flags); \
} while (0)

static void sla_hash_command(bool clear)
{
    unsigned long flags;

    spin_lock_irqsave(&stats_spin_lock, flags);
    spin_lock(&pkt_spin_lock);
    if (clear)
        pkt_hlist_deinit();
    else
        pkt_hlist_print();
    spin_unlock(&pkt_spin_lock);
    spin_unlock_irqrestore(&stats_spin_lock, flags);
}

static void sla_clear_voip(void)
{
    unsigned long flags;

    spin_lock_irqsave(&stats_spin_lock, flags);
    memset(&voip_state, 0, sizeof(voip_state));
    spin_unlock_irqrestore(&stats_spin_lock, flags);
}

static ssize_t config_file_write_locked(struct file *filp, const char __user *buffer,  size_t count, loff_t *ppos)
{
    char cmd[CONFIG_CMD_MAX + 1] = {0};
    unsigned long num = 0;
    int len = 0;
    char *p = NULL;
    char *buf = NULL;

    if (!count)
        return 0;

    if(count > CONFIG_CMD_MAX)
        return -EFAULT;

    if(copy_from_user(cmd, buffer, count))
        return -EFAULT;

    cmd[count] = 0;
    if (cmd[count - 1] == '\n')
        cmd[count - 1] = 0;

    len = strlen(CMD_IFACE_ADD);
    if(0 == strncmp(cmd, CMD_IFACE_ADD, len))
    {
        printk("#### %s CMD_IFACE_ADD=[%s]\n", __func__, cmd+len);
        buf = cmd+len;
        if (buf[0] && strlen(buf) < IF_NAME_LEN &&
            !strchr(buf, '/') && !strpbrk(buf, " \t\r\n") &&
            strcmp(buf, ".") && strcmp(buf, ".."))
            sla_interface_start((const char*)buf);
        else
            printk(KERN_ERR "#### %s ifname=[%s] is invaild\n", __func__, buf);

        return count;
    }

    len = strlen(CMD_ENABLE);
    if(0 == strncmp(cmd, CMD_ENABLE, len))
    {
        printk("#### %s enable=%c\n", __func__, cmd[len]);
        if('1' == cmd[len] && 0 == cmd[len+1])
        {
            sla_stats_enable();
        }
        else if('0' == cmd[len] && 0 == cmd[len+1])
        {
            sla_stats_disable();
        }

        return count;
    }

    len = strlen(CMD_RATE_ON);
    if(0 == strncmp(cmd, CMD_RATE_ON, len))
    {
        printk("#### %s rate_on=%c\n", __func__, cmd[len]);
        if('1' == cmd[len] && 0 == cmd[len+1])
            SLA_CONFIG_SET(rate_on, 1);
        else if('0' == cmd[len] && 0 == cmd[len+1])
            SLA_CONFIG_SET(rate_on, 0);

        return count;
    }


    len = strlen(CMD_PORTS);
    if (!strncmp(cmd, CMD_PORTS, len)) {
        u16 ports[PORTS_MAX] = {0};
        unsigned long flags;
        unsigned int used = 0;

        buf = cmd + len;
        if (*buf) {
            while ((p = strsep(&buf, ",")) != NULL) {
                if (!*p || kstrtoul(p, 10, &num) ||
                    !num || num > 0xffff)
                    return -EINVAL;
                if (used == PORTS_MAX)
                    return -E2BIG;
                ports[used++] = num;
            }
        }

        spin_lock_irqsave(&stats_spin_lock, flags);
        memcpy(config_data.ports, ports, sizeof(ports));
        spin_unlock_irqrestore(&stats_spin_lock, flags);
        return count;
    }

    len = strlen(CMD_RESET_TRAFFIC);
    if(0 == strncmp(cmd, CMD_RESET_TRAFFIC, len))
    {
        if('1' == cmd[len] && 0 == cmd[len+1])
        {
            struct sla_interface * iface = NULL;
            unsigned long flags;
            STATS_LOCK_IRQ(&stats_spin_lock,flags);
            list_for_each_entry (iface , &sla_iface_list, interface_list_hook)
            {
                iface->total_trfc = 0;
            }
            STATS_UNLOCK_IRQ(&stats_spin_lock,flags);
            printk("#### %s reset_traffic=%c\n", __func__, cmd[len]);

        }

        return count;
    }

    len = strlen(CMD_HASH_PRINT);
    if(0 == strncmp(cmd, CMD_HASH_PRINT, len))
    {
        printk("#### %s hash print %llu, %llu\n", __func__, pkt_alloc_num, pkt_free_num);
        sla_hash_command(false);

        return count;
    }

    len = strlen(CMD_HASH_CLEAR);
    if(0 == strncmp(cmd, CMD_HASH_CLEAR, len))
    {
        printk("#### %s hash clear\n", __func__);
        sla_hash_command(true);

        return count;
    }

    len = strlen(CMD_MAX_SIZE);
    if(0 == strncmp(cmd, CMD_MAX_SIZE, len))
    {
        if (kstrtoul(&cmd[len], 10, &num))
            return -EINVAL;
        if (num > 0xffff)
            return -ERANGE;
        if(num < MAX_SIZE_DEFAULT)
        {
            printk(KERN_ERR "#### %s max_size=%lu is invaild, need greater than %d\n", __func__, num, MAX_SIZE_DEFAULT);
        }
        else
        {
            SLA_CONFIG_SET(max_size, num & 0xFFFF);
            printk("#### %s max_size=%d\n", __func__, config_data.max_size);
        }

        return count;
    }

    len = strlen(CMD_LIMIT_TIME);
    if(0 == strncmp(cmd, CMD_LIMIT_TIME, len))
    {
        if (kstrtoul(&cmd[len], 10, &num))
            return -EINVAL;
        if (num > 0xffff)
            return -ERANGE;
        if(num < LIMIT_TIME_DEFAULT)
        {
            printk(KERN_ERR "#### %s limit_time=%lu is invaild, need longer than %d\n", __func__, num, LIMIT_TIME_DEFAULT);
        }
        else
        {
            SLA_CONFIG_SET(limit_time, num & 0xFFFF);
            printk("#### %s limit_time=%d\n", __func__, config_data.limit_time);
        }

        return count;
    }

    len = strlen(CMD_VOIP_ENABLE);
    if(0 == strncmp(cmd, CMD_VOIP_ENABLE, len)) {
        printk("#### %s voip_enbale=%s\n", __func__, cmd+len);
        if('1' == cmd[len] && 0 == cmd[len+1]) {
            sla_clear_voip();
            if (!sla_voip_file) {
                printk(KERN_ERR "#### %s Fail to open file: sla_voip_file\n", __func__);
                return 1;
            }
            SLA_CONFIG_SET(voip_enable, 1);
        } else if('0' == cmd[len] && 0 == cmd[len+1]) {
            SLA_CONFIG_SET(voip_enable, 0);
        }

        return count;
    }

    len = strlen(CMD_VOIP_UID);
    if(0 == strncmp(cmd, CMD_VOIP_UID, len)) {
        printk("#### %s voip_uid=%s\n", __func__, cmd+len);
        if (kstrtoul(cmd + len, 10, &num) || num > U32_MAX)
            return -EINVAL;
        SLA_CONFIG_SET(voip_uid, num);
        return count;
    }

    return count;
}
static ssize_t config_file_write(struct file *filp,
                                  const char __user *buffer,
                                  size_t count, loff_t *ppos)
{
    ssize_t ret;

    mutex_lock(&config_mutex);
    ret = config_file_write_locked(filp, buffer, count, ppos);
    mutex_unlock(&config_mutex);
    return ret;
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
static const struct proc_ops config_fops =
{
    .proc_open = config_file_open,
    .proc_read = seq_read,
    .proc_write = config_file_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};
#else
static const struct file_operations config_fops =
{
    .owner = THIS_MODULE,
    .open = config_file_open,
    .read = seq_read,
    .write = config_file_write,
    .llseek = seq_lseek,
    .release = single_release,
};
#endif

static int __init sla_static_collector_init(void)
{
    int ret;

    BUILD_BUG_ON(sizeof(struct sla_stats_entry) != 16);
    BUILD_BUG_ON(sizeof(struct sla_interface_stats) != 20480);
    BUILD_BUG_ON(sizeof(struct sla_config) != 268);

    list_lock_init();
    stats_lock_init();
    pkt_lock_init();
    pkt_hlist_init();

    memset(&config_data, 0, sizeof(config_data));
    config_data.ports[0] = 80;
    config_data.ports[1] = 443;
    config_data.max_size = MAX_SIZE_DEFAULT;
    config_data.limit_time = LIMIT_TIME_DEFAULT;
    memset(&voip_state, 0, sizeof(voip_state));

    ret = nf_register_net_hooks(&init_net, sla_ipv4_ops,
                                 ARRAY_SIZE(sla_ipv4_ops));
    if (ret)
        return ret;

    sla_dir = proc_mkdir("sla", NULL);
    if (!sla_dir) {
        ret = -ENOMEM;
        goto unregister;
    }
    config_file = proc_create_data("config", 0644, sla_dir,
                                   &config_fops, &config_data);
    if (!config_file) {
        ret = -ENOMEM;
        goto remove;
    }
    sla_voip_file = proc_create_data(VOIP_STATS_FILE, 0644, sla_dir,
                                     &sla_voip_fops, &voip_state);
    if (!sla_voip_file) {
        ret = -ENOMEM;
        proc_remove(config_file);
        config_file = NULL;
        goto remove;
    }

    pr_info("sla: Spring statistics collector initialized\n");
    return 0;

remove:
    mutex_lock(&config_mutex);
    sla_stats_disable();
    proc_remove(sla_dir);
    sla_dir = NULL;
    mutex_unlock(&config_mutex);
unregister:
    nf_unregister_net_hooks(&init_net, sla_ipv4_ops,
                             ARRAY_SIZE(sla_ipv4_ops));
    return ret;
}

static void __exit sla_static_collector_deinit(void)
{
    nf_unregister_net_hooks(&init_net, sla_ipv4_ops,
                             ARRAY_SIZE(sla_ipv4_ops));

    /* Drain control callbacks before taking their mutex. */
    proc_remove(config_file);
    proc_remove(sla_voip_file);

    mutex_lock(&config_mutex);
    sla_stats_disable();
    proc_remove(sla_dir);
    mutex_unlock(&config_mutex);
}


MODULE_LICENSE("GPL v2");
module_init(sla_static_collector_init);
module_exit(sla_static_collector_deinit);

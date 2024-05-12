#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"

#include "util.h"
#include "net.h"
#include "ip.h"

struct net_protocol {
    struct net_protocol *next;
    uint16_t type;
    struct queue_head queue; /* input queue */
    void (*handler)(const uint8_t *data, size_t len, struct net_device *dev);
};

struct net_protocol_queue_entry {
    struct net_device *dev;
    size_t len;
    uint8_t data[];
};

static struct net_device *devices;
static struct net_protocol *protocols;

struct net_device *
net_device_alloc(void)
{
    struct net_device *dev;

    dev = memory_alloc(sizeof(*dev));
    if (!dev) {
        errorf("memory_alloc failed\n");
        return NULL;
    }
    return dev;
}

int 
net_device_register(struct net_device *dev)
{
    static unsigned int index = 0;

    dev->index = index++;
    snprintf(dev->name, sizeof(dev->name), "net%d", dev->index);
    dev->next = devices;
    devices = dev;
    infof("registerd, dec=%s, type=0x%04x, dev->name, dev->type");
    return 0;
}

static int
net_device_open(struct net_device *dev)
{
    if (NET_DEVICE_IS_UP(dev)) {
        errorf("device %s is already up\n", dev->name);
        return -1;
    }
    if (dev->ops->open) {
        if (dev->ops->open(dev) < 0) {
            errorf("failure, dev=%s\n", dev->name);
            return -1;
        }
    }
    dev->flags |= NET_DEVICE_FLAG_UP;
    infof("dev=%s, state=%s\n", dev->name, NET_DEVICE_STATE(dev));
    return 0;
}

static int
net_device_close(struct net_device *dev)
{
    if (!NET_DEVICE_IS_UP(dev)) {
        errorf("device %s is already down\n", dev->name);
        return -1;
    }
    if (dev->ops->close) {
        if (dev->ops->close(dev) < 0) {
            errorf("failure, dev=%s\n", dev->name);
            return -1;
        }
    }
    dev->flags &= ~NET_DEVICE_FLAG_UP;
    infof("dev=%s, state=%s\n", dev->name, NET_DEVICE_STATE(dev));
    return 0;
}

int
net_device_output(struct net_device *dev, uint16_t type, const uint8_t *data, size_t len, const void *dst)
{
    if (!NET_DEVICE_IS_UP(dev)) {
        errorf("device %s is down\n", dev->name);
        return -1;
    }
    if (len > dev->mtu) {
        errorf("too long packet, dev=%s, len=%zu, mtu=%u\n", dev->name, len, dev->mtu);
        return -1;
    }
    debugf("dev=%s, type=0x%04x, len=%zu\n", dev->name, type, len);
    debugdump(data, len);
    if (dev->ops->transmit(dev, type, data, len, dst) < 0) {
        errorf("failure, dev=%s\n", dev->name);
        return -1;
    }
    return 0; 
}

/* NOTE: must not be call after net_run() */
int
net_protocol_register(uint16_t type, void (*handler)(const uint8_t *data, size_t len, struct net_device *dev))
{
    struct net_protocol *proto;

    for (proto = protocols; proto; proto = proto->next) {
        if (type == proto->type) {
            errorf("already registered, type=0x%04x", type);
            return -1;
        }
    }
    proto = memory_alloc(sizeof(*proto));
    if (!proto) {
        errorf("memory_alloc() failure");
        return -1;
    }
    proto->type = type;
    proto->handler = handler;
    proto->next = protocols;
    protocols = proto;
    infof("registered, type=0x%04x", type);
    return 0;
}


int
net_input_handler(uint16_t type, const uint8_t *data, size_t len, struct net_device *dev)
{
    struct net_protocol *proto;
    struct net_protocol_queue_entry *entry;

    for (proto = protocols; proto; proto = proto->next) {
        if (type == proto->type) {
            // 新しいエントリのメモリを確保
            entry = memory_alloc(sizeof(*entry) + len);
            if (!entry) {
                errorf("memory_alloc failed\n");
                return -1;
            }
        // 新しいエントリのメタデータの設定と受信データのコピー
        entry->dev = dev;
        entry->len = len;
        memcpy(entry->data, data, len); // data から entry->data へ len バイトのデータをコピー
        // キューにエントリを追加
        queue_push(&proto->queue, entry);
        debugf("queue pushed (num=%u), type=0x%04x, len=%zu\n", proto->queue.num, type, len);
        debugdump(data, len);
        return 0;
        }
    }
    /* unsupported protocol */
    return 0;
}

int
net_run(void)
{
    struct net_device *dev;

    if (intr_run() == -1) {
        errorf("intr_run failed\n");
        return -1;
    }

    debugf("open all devices\n");
    for (dev = devices; dev; dev = dev->next) {
        net_device_open(dev);
    }
    debugf("running...\n");
    return 0;
}

void
net_shutdown(void)
{
    struct net_device *dev;

    debugf("close all devices\n");
    for (dev = devices; dev; dev = dev->next) {
        net_device_close(dev);
    }
    intr_shutdown();
    debugf("shuting down\n");
}

int
net_init(void)
{
    if (intr_init() == -1) {
        errorf("intr_init failed\n");
        return -1;
    }
    if (ip_init() == -1) {
        errorf("ip_init failed\n");
        return -1;
    }
    infof("initializing...\n");
    return 0;
}
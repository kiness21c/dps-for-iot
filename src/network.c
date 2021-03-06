/*
 *******************************************************************
 *
 * Copyright 2016 Intel Corporation All rights reserved.
 *
 *-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 */

#include <assert.h>
#include <string.h>
#include <malloc.h>
#include <uv.h>
#include <dps/dbg.h>
#include <dps/dps.h>
#include <dps/private/network.h>
#include "node.h"

/*
 * Debug control for this module
 */
DPS_DEBUG_CONTROL(DPS_DEBUG_ON);

const char* DPS_NetAddrText(const struct sockaddr* addr)
{
    if (addr) {
        char name[INET6_ADDRSTRLEN];
        static char txt[sizeof(name) + 8];
        uint16_t port;
        int ret;
        if (addr->sa_family == AF_INET6) {
            ret = uv_ip6_name((const struct sockaddr_in6*)addr, name, sizeof(name));
            port = ((const struct sockaddr_in6*)addr)->sin6_port;
        } else {
            ret = uv_ip4_name((const struct sockaddr_in*)addr, name, sizeof(name));
            port = ((const struct sockaddr_in*)addr)->sin_port;
        }
        if (ret) {
            return "Invalid address";
        }
        /*
         * Make sure name is NUL terminated
         */
        name[sizeof(name) - 1] = 0;
        snprintf(txt, sizeof(txt), "%s/%d", name, ntohs(port));
        return txt;
    } else {
        return "NULL";
    }
}

static const uint8_t IP4as6[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0 };

int DPS_SameAddr(DPS_NodeAddress* addr1, DPS_NodeAddress* addr2)
{
    struct sockaddr* a = (struct sockaddr*)&addr1->inaddr;
    struct sockaddr* b = (struct sockaddr*)&addr2->inaddr;
    struct sockaddr_in6 tmp;

    if (a->sa_family != b->sa_family) {
        uint32_t ip;
        if (a->sa_family == AF_INET6) {
            struct sockaddr_in* ipb = (struct sockaddr_in*)b;
            ip = ipb->sin_addr.s_addr;
            tmp.sin6_port = ipb->sin_port;
            b = (struct sockaddr*)&tmp;
        } else {
            struct sockaddr_in* ipa = (struct sockaddr_in*)a;
            ip = ipa->sin_addr.s_addr;
            tmp.sin6_port = ipa->sin_port;
            a = (struct sockaddr*)&tmp;
        }
        memcpy_s(&tmp.sin6_addr, sizeof(tmp.sin6_addr), IP4as6, 12);
        memcpy_s((uint8_t*)&tmp.sin6_addr + 12, sizeof(tmp.sin6_addr) - 12, &ip, 4);
        tmp.sin6_family = AF_INET6;
    }
    if (a->sa_family == AF_INET6) {
        struct sockaddr_in6* ip6a = (struct sockaddr_in6*)a;
        struct sockaddr_in6* ip6b = (struct sockaddr_in6*)b;
        return (ip6a->sin6_port == ip6b->sin6_port) && (memcmp(&ip6a->sin6_addr, &ip6b->sin6_addr, 16) == 0);
    } else {
        struct sockaddr_in* ipa = (struct sockaddr_in*)a;
        struct sockaddr_in* ipb = (struct sockaddr_in*)b;
        return (ipa->sin_port == ipb->sin_port) && (ipa->sin_addr.s_addr == ipb->sin_addr.s_addr);
    }
}

#define MAX_HOST_LEN    256  /* Per RFC 1034/1035 */
#define MAX_SERVICE_LEN  16  /* Per RFC 6335 section 5.1 */

typedef struct {
    DPS_Node* node;
    uv_async_t async;
    DPS_OnResolveAddressComplete cb;
    void* data;
    uv_getaddrinfo_t info;
    char host[MAX_HOST_LEN + 1];
    char service[MAX_SERVICE_LEN + 1];
} ResolverInfo;

static void FreeHandle(uv_handle_t* handle)
{
    if (handle->data) {
        free(handle->data);
    }
}

static void GetAddrInfoCB(uv_getaddrinfo_t* req, int status, struct addrinfo* res)
{
    ResolverInfo* resolver = (ResolverInfo*)req->data;
    if (status == 0) {
        DPS_NodeAddress addr;
        if (res->ai_family == AF_INET6) {
            memcpy_s(&addr.inaddr, sizeof(addr.inaddr), res->ai_addr, sizeof(struct sockaddr_in6));
        } else {
            memcpy_s(&addr.inaddr, sizeof(addr.inaddr), res->ai_addr, sizeof(struct sockaddr_in));
        }
        resolver->cb(resolver->node, &addr, resolver->data);
        uv_freeaddrinfo(res);
    } else {
        DPS_ERRPRINT("uv_getaddrinfo failed %s\n", uv_err_name(status));
        resolver->cb(resolver->node, NULL, resolver->data);
    }
    assert(resolver->async.data == resolver);
    resolver->async.data = resolver;
    uv_close((uv_handle_t*)&resolver->async, FreeHandle);
}

static void AsyncResolveAddress(uv_async_t* async)
{
    ResolverInfo* resolver = (ResolverInfo*)async->data;
    int r;
    struct addrinfo hints;

    DPS_DBGTRACE();

    memzero_s(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    resolver->info.data = resolver;
    r = uv_getaddrinfo(async->loop, &resolver->info, GetAddrInfoCB, resolver->host, resolver->service, &hints);
    if (r) {
        DPS_ERRPRINT("uv_getaddrinfo call error %s\n", uv_err_name(r));
        resolver->cb(resolver->node, NULL, resolver->data);
        uv_close((uv_handle_t*)async, FreeHandle);
    }
}

DPS_Status DPS_ResolveAddress(DPS_Node* node, const char* host, const char* service, DPS_OnResolveAddressComplete cb, void* data)
{
    int r;
    ResolverInfo* resolver;

    DPS_DBGTRACE();

    if (!node->loop) {
        DPS_ERRPRINT("Cannot resolve address - node has not been started\n");
        return DPS_ERR_INVALID;
    }
    if (!service || !cb) {
        return DPS_ERR_NULL;
    }
    if (!host) {
        host = "localhost";
    }
    resolver = calloc(1, sizeof(ResolverInfo));
    if (!resolver) {
        return DPS_ERR_RESOURCES;
    }
    strncpy_s(resolver->host, sizeof(resolver->host), host, sizeof(resolver->host) - 1);
    strncpy_s(resolver->service, sizeof(resolver->service), service, sizeof(resolver->service) - 1);
    resolver->node = node;
    resolver->cb = cb;
    resolver->data = data;
    /*
     * Async callback
     */
    r = uv_async_init(node->loop, &resolver->async, AsyncResolveAddress);
    if (r) {
        free(resolver);
        return DPS_ERR_RESOURCES;
    }
    resolver->async.data = resolver;
    r = uv_async_send(&resolver->async);
    if (r) {
        uv_close((uv_handle_t*)&resolver->async, FreeHandle);
        return DPS_ERR_FAILURE;
    } else {
        return DPS_OK;
    }
}

DPS_NodeAddress* DPS_SetAddress(DPS_NodeAddress* addr, const struct sockaddr* sa)
{
    memzero_s(addr, sizeof(DPS_NodeAddress));
    if (sa) {
        if (sa->sa_family == AF_INET) {
            memcpy_s(&addr->inaddr, sizeof(addr->inaddr), sa, sizeof(struct sockaddr_in));
        } else if (sa->sa_family == AF_INET6) {
            memcpy_s(&addr->inaddr, sizeof(addr->inaddr), sa, sizeof(struct sockaddr_in6));
        }
    }
    return addr;
}

void DPS_EndpointSetPort(DPS_NetEndpoint* ep, uint16_t port)
{
    port = htons(port);
    if (ep->addr.inaddr.ss_family == AF_INET6) {
        struct sockaddr_in6* ip6 = (struct sockaddr_in6*)&ep->addr.inaddr;
        ip6->sin6_port = port;
    } else {
        struct sockaddr_in* ip4 = (struct sockaddr_in*)&ep->addr.inaddr;
        ip4->sin_port = port;
    }
}

void DPS_NetFreeBufs(uv_buf_t* bufs, size_t numBufs)
{
    while (numBufs--) {
        if (bufs->base) {
            free(bufs->base);
        }
        ++bufs;
    }
}


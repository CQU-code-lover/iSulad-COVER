/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2020. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: lifeng
 * Create: 2020-06-22
 * Description: provide container events handler definition
 ******************************************************************************/
#include <stdlib.h>
#include <pthread.h>
#include <isula_libutils/container_config_v2.h>
#include <isula_libutils/host_config.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/prctl.h>

#include "isula_libutils/log.h"
#include "container_events_handler.h"
#include "utils.h"
#include "container_api.h"
#include "service_container_api.h"
#include "plugin_api.h"
#include "restartmanager.h"
#include "err_msg.h"
#include "events_format.h"
#include "linked_list.h"
#include "utils_timestamp.h"

 /* events handler lock */
static void events_handler_lock(container_events_handler_t *handler)
{
    if (pthread_mutex_lock(&(handler->mutex)) != 0) {
        ERROR("Failed to lock events handler");
    }
}
//操作handler时互斥锁包装
/* events handler unlock */
static void events_handler_unlock(container_events_handler_t *handler)
{
    if (pthread_mutex_unlock(&(handler->mutex)) != 0) {
        ERROR("Failed to unlock events handler");
    }
}

//将handler中的申请的内存释放
/* events handler free */
void container_events_handler_free(container_events_handler_t *handler)
{
    struct isulad_events_format *event = NULL;
    struct linked_list *it = NULL;
    struct linked_list *next = NULL;

    if (handler == NULL) {
        return;
    }

    //遍历handler的event_list 并且将这个链表的每个节点的内存都释放
    linked_list_for_each_safe(it, &(handler->events_list), next) {
        event = (struct isulad_events_format *)it->elem;
        linked_list_del(it);
        isulad_events_format_free(event);
        free(it);
        it = NULL;
    }
    //如果互斥锁已经被注册过了 需要销毁这个互斥锁
    if (handler->init_mutex) {
        pthread_mutex_destroy(&(handler->mutex));
    }
    free(handler);
}

//创建event handler
/* events handler new */
container_events_handler_t *container_events_handler_new()
{
    int ret;
    container_events_handler_t *handler = NULL;
    //为handler分配内存空间
    handler = util_common_calloc_s(sizeof(container_events_handler_t));
    if (handler == NULL) {
        ERROR("Out of memory");
        return NULL;
    }
    //初始化handler的访问互斥锁
    ret = pthread_mutex_init(&(handler->mutex), NULL);
    if (ret != 0) {
        ERROR("Failed to init mutex of events_handler");
        goto cleanup;
    }

    //将handler互斥锁初始化标志设置为true 这个标志在handler free时用来判断是否需要销毁互斥锁
    handler->init_mutex = true;

    //初始化handler的event list
    linked_list_init(&(handler->events_list));

    handler->has_handler = false;

    return handler;
cleanup:
    container_events_handler_free(handler);
    return NULL;
}

//根据传入的event来改变传入的container的状态
/* container state changed */
static int container_state_changed(container_t *cont, const struct isulad_events_format *events)
{
    int ret = 0;
    int pid = 0;
    uint64_t timeout;
    char *id = events->id;
    char *started_at = NULL;
    bool should_restart = false;
    bool auto_remove = false;

    /* only handle Exit event */
    if (events->type != EVENTS_TYPE_STOPPED1) {
        return 0;
    }

    //此处只处理stopped事件
    switch (events->type) {
        case EVENTS_TYPE_STOPPED1:
            //获取container锁
            container_lock(cont);

            if (false == container_is_running(cont->state)) {
                DEBUG("Container is not in running state ignore STOPPED event");
                container_unlock(cont);
                ret = 0;
                goto out;
            }

            //获取容器运行的进程id
            pid = container_state_get_pid(cont->state);
            //判断事件的目标pid是否与本容器的pid相同 如果相同 表示为本容器为事件处理的目标容器
            if (pid != (int)events->pid) {
                DEBUG("Container's pid \'%d\' is not equal to event's pid \'%d\', ignore STOPPED event", pid,
                      events->pid);
                container_unlock(cont);
                ret = 0;
                goto out;
            }

            started_at = container_state_get_started_at(cont->state);

            //从container的common conf配置中获取是否设置了容器重启标志
            should_restart = restart_manager_should_restart(id, events->exit_status,
                                                            cont->common_config->has_been_manually_stopped,
                                                            time_seconds_since(started_at), &timeout);
            free(started_at);
            started_at = NULL;

            if (should_restart) {
                //如果设置了容器重启 则进行重启操作
       
                cont->common_config->restart_count++;
                //将容器状态设置为重启
                container_state_set_restarting(cont->state, (int)events->exit_status);
                //??
                container_wait_stop_cond_broadcast(cont);
                INFO("Try to restart container %s after %.2fs", id, (double)timeout / Time_Second);
                (void)container_restart_in_thread(id, timeout, (int)events->exit_status);
            } else {
                //否则停止容器
                container_state_set_stopped(cont->state, (int)events->exit_status);
                container_wait_stop_cond_broadcast(cont);
                plugin_event_container_post_stop(cont);
                container_stop_health_checks(cont->common_config->id);
            }

            auto_remove = !should_restart && cont->hostconfig != NULL && cont->hostconfig->auto_remove;
            if (auto_remove) {
                //将容器状态设置为移除
                ret = set_container_to_removal(cont);
                if (ret != 0) {
                    ERROR("Failed to set container %s state to removal", cont->common_config->id);
                }
            }

            //将容器信息保存到硬盘
            if (container_to_disk(cont)) {
                container_unlock(cont);
                ERROR("Failed to save container \"%s\" to disk", id);
                ret = -1;
                goto out;
            }

            container_unlock(cont);

            //如果设置了主动移除 会自动进行容器的删除操作
            if (auto_remove) {
                //会调用do_delete_container会删除runtime、运行目录结构、镜像层等资源 但是不会删除container store中的container描述结构
                ret = delete_container(cont, true);
                if (ret != 0) {
                    ERROR("Failed to cleanup container %s", cont->common_config->id);
                    ret = -1;
                    goto out;
                }
            }

            break;
        default:
            /* ignore garbage */
            break;
    }
out:
    return ret;
}

static int handle_one(container_t *cont, container_events_handler_t *handler)
{
    struct linked_list *it = NULL;
    struct isulad_events_format *events = NULL;

    events_handler_lock(handler);

    if (linked_list_empty(&(handler->events_list))) {
        handler->has_handler = false;
        events_handler_unlock(handler);
        return -1;
    }

    it = linked_list_first_node(&(handler->events_list));
    linked_list_del(it);

    events_handler_unlock(handler);

    events = (struct isulad_events_format *)it->elem;
    INFO("Received event %s with pid %d", events->id, events->pid);

    //进行事件的处理
    if (container_state_changed(cont, events)) {
        ERROR("Failed to change container %s state", cont->common_config->id);
    }

    isulad_events_format_free(events);
    events = NULL;

    free(it);
    it = NULL;

    return 0;
}

//用于处理event的线程
//参数为容器name 每个容器都有一个event handler线程
/* events handler thread */
static void *events_handler_thread(void *args)
{
    int ret = 0;
    char *name = args;
    container_t *cont = NULL;
    container_events_handler_t *handler = NULL;
    //将本线程脱离阻塞
    ret = pthread_detach(pthread_self());
    if (ret != 0) {
        CRIT("Set thread detach fail");
        goto out;
    }

    prctl(PR_SET_NAME, "events_handler");

    cont = containers_store_get(name);
    if (cont == NULL) {
        INFO("Container '%s' already removed", name);
        goto out;
    }

    handler = cont->handler;
    if (handler == NULL) {
        INFO("Container '%s' event handler already removed", name);
        goto out;
    }

    //循环执行来监听处理事件
    while (handle_one(cont, handler) == 0) {
    }

out:
    container_unref(cont);
    free(name);
    DAEMON_CLEAR_ERRMSG();
    return NULL;
}

//推送event
/* events handler post events */
int container_events_handler_post_events(const struct isulad_events_format *event)
{
    int ret = 0;
    char *name = NULL;
    pthread_t td;
    struct isulad_events_format *post_event = NULL;
    struct linked_list *it = NULL;
    container_t *cont = NULL;

    if (event == NULL) {
        return -1;
    }

    cont = containers_store_get(event->id);
    if (cont == NULL) {
        ERROR("No such container:%s", event->id);
        ret = -1;
        goto out;
    }

    it = util_common_calloc_s(sizeof(struct linked_list));
    if (it == NULL) {
        ERROR("Failed to malloc for linked_list");
        ret = -1;
        goto out;
    }

    linked_list_init(it);

    //复制event并存放到新的内存空间
    post_event = dup_event(event);
    if (post_event == NULL) {
        ERROR("Failed to dup event");
        ret = -1;
        goto out;
    }

    linked_list_add_elem(it, post_event);
    post_event = NULL;

    events_handler_lock(cont->handler);

    //将event节点添加到container的handler list中
    linked_list_add_tail(&(cont->handler->events_list), it);
    it = NULL;

    //如果container的has_handler为false 表示没有处理过event 所以没有相应的handler线程 故应该创建线程
    if (cont->handler->has_handler == false) {
        name = util_strdup_s(event->id);
        ret = pthread_create(&td, NULL, events_handler_thread, name);
        if (ret) {
            CRIT("Events handler thread create failed");
            free(name);
            goto out;
        }
        cont->handler->has_handler = true;
    }
out:
    free(it);
    isulad_events_format_free(post_event);
    events_handler_unlock(cont->handler);
    container_unref(cont);
    return ret;
}

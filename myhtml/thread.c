//
//  myhtml_thread.c
//  myhtml
//
//  Created by Alexander Borisov on 02.10.15.
//  Copyright (c) 2015 Alexander Borisov. All rights reserved.
//

#include "thread.h"

mythread_t * mythread_create(void)
{
    return calloc(1, sizeof(mythread_t));
}

myhtml_status_t mythread_init(mythread_t *mythread, const char *sem_prefix, size_t thread_count)
{
    mythread->batch_count    = 0;
    mythread->batch_first_id = 0;
    mythread->stream_opt     = MyTHREAD_OPT_WAIT;
    mythread->batch_opt      = MyTHREAD_OPT_WAIT;
    
    if(thread_count)
    {
        mythread->attr = (pthread_attr_t*)calloc(1, sizeof(pthread_attr_t));
        
        if(mythread->attr == NULL)
            return MyHTML_STATUS_THREAD_ERROR_ATTR_MALLOC;
        
        mythread->sys_last_error = pthread_attr_init(mythread->attr);
        if(mythread->sys_last_error)
            return MyHTML_STATUS_THREAD_ERROR_ATTR_INIT;
        
        mythread->sys_last_error = pthread_attr_setdetachstate(mythread->attr, PTHREAD_CREATE_JOINABLE);
        
        if(mythread->sys_last_error)
            return MyHTML_STATUS_THREAD_ERROR_ATTR_SET;
        
        mythread->pth_list_root   = 1;
        mythread->pth_list_length = 1;
        mythread->pth_list_size   = thread_count + 1;
        mythread->pth_list        = (mythread_list_t*)calloc(mythread->pth_list_size, sizeof(mythread_list_t));
        
        if(mythread->pth_list == NULL)
            return MyHTML_STATUS_THREAD_ERROR_LIST_INIT;
    }
    else {
        mythread->attr            = NULL;
        mythread->sys_last_error  = 0;
        mythread->pth_list_root   = 1;
        mythread->pth_list_length = 1;
        mythread->pth_list_size   = 0;
        mythread->pth_list        = NULL;
    }
    
    myhtml_status_t status;
    mythread->queue = mythread_queue_create(4096, &status);
    
    if(mythread->queue == NULL)
        return status;
    
    if(sem_prefix)
    {
        mythread->sem_prefix_length = strlen(sem_prefix) + 1;
        
        if(mythread->sem_prefix_length) {
            mythread->sem_prefix = calloc(mythread->sem_prefix_length, sizeof(char));
            
            if(mythread->sem_prefix == NULL) {
                mythread->sem_prefix_length = 0;
                return MyHTML_STATUS_THREAD_ERROR_SEM_PREFIX_MALLOC;
            }
            
            strcpy(mythread->sem_prefix, sem_prefix);
        }
    }
    
    return MyHTML_STATUS_OK;
}

void mythread_clean(mythread_t *mythread)
{
    mythread->sys_last_error = 0;
    
    if(mythread->queue)
        mythread_queue_clean(mythread->queue);
    
    size_t idx;
    for (idx = mythread->pth_list_root; idx < mythread->pth_list_length; idx++) {
        mythread->pth_list[idx].data.use = 0;
    }
    
    for (idx = 0; idx < mythread->batch_count; idx++) {
        mythread->pth_list[( mythread->batch_first_id + idx )].data.use = idx;
    }
}

mythread_t * mythread_destroy(mythread_t *mythread, mybool_t self_destroy)
{
    if(mythread == NULL)
        return NULL;
    
    if(mythread->attr) {
        pthread_attr_destroy(mythread->attr);
        
        free(mythread->attr);
        mythread->attr = NULL;
    }
    
    if(mythread->pth_list) {
        mythread_resume_all(mythread);
        mythread_stream_quit_all(mythread);
        mythread_batch_quit_all(mythread);
        mythread_wait_all(mythread);
        
        for (size_t i = mythread->pth_list_root; i < mythread->pth_list_length; i++)
        {
            pthread_join(mythread->pth_list[i].pth, NULL);
            
            if(mythread->pth_list[i].data.sem_name) {
                free(mythread->pth_list[i].data.sem_name);
            }
        }
        
        free(mythread->pth_list);
        mythread->pth_list = NULL;
    }
    
    if(mythread->sem_prefix) {
        free(mythread->sem_prefix);
        
        mythread->sem_prefix = NULL;
        mythread->sem_prefix_length = 0;
    }
    
    if(mythread->queue)
        mythread_queue_destroy(mythread->queue);
    
    if(self_destroy) {
        free(mythread);
        return NULL;
    }
    
    return mythread;
}

myhtml_status_t myhread_create_sem(mythread_t *mythread, mythread_context_t *ctx, size_t prefix_id)
{
    ctx->sem_name = calloc(1024, sizeof(char));
    
    sprintf(ctx->sem_name, "%s%lus%lup%lu", MyTHREAD_SEM_NAME, prefix_id, ctx->id, (size_t)mythread);
    
    ctx->sem_name_size = strlen(ctx->sem_name);
    
    ctx->sem = sem_open(ctx->sem_name, O_CREAT, S_IRWXU|S_IRWXG, 0);
    
    if(ctx->sem == SEM_FAILED) {
        free(ctx->sem_name);
        
        mythread->sys_last_error = errno;
        return MyHTML_STATUS_THREAD_ERROR_SEM_CREATE;
    }
    
    return MyHTML_STATUS_OK;
}

mythread_id_t _myhread_create_stream_raw(mythread_t *mythread, mythread_f func, void *work_func, myhtml_status_t *status, size_t total_count)
{
    mythread->sys_last_error = 0;
    
    if(status)
        *status = MyHTML_STATUS_OK;
    
    if(mythread->pth_list_length >= mythread->pth_list_size) {
        if(status)
            *status = MyHTML_STATUS_THREAD_ERROR_NO_SLOTS;
        
        return 0;
    }
    
    mythread_list_t *thr = &mythread->pth_list[mythread->pth_list_length];
    
    thr->data.mythread = mythread;
    thr->data.func     = func;
    thr->data.id       = mythread->pth_list_length;
    thr->data.use      = 0;
    thr->data.qnode    = NULL;
    thr->data.t_count  = total_count;
    
    myhtml_status_t m_status = myhread_create_sem(mythread, &thr->data, 0);
    
    if(m_status != MyHTML_STATUS_OK && status) {
        *status = m_status;
        return 0;
    }
    
    pthread_create(&thr->pth, mythread->attr,
                   work_func,
                   (void*)(&thr->data));
    
    mythread->pth_list_length++;
    return thr->data.id;
}

mythread_id_t myhread_create_stream(mythread_t *mythread, mythread_f func, myhtml_status_t *status)
{
    return _myhread_create_stream_raw(mythread, func, mythread_function_stream, status, 0);
}

mythread_id_t myhread_create_batch(mythread_t *mythread, mythread_f func, myhtml_status_t *status, size_t count)
{
    if(mythread->batch_count) {
        *status = MyHTML_STATUS_THREAD_ERROR_BATCH_INIT;
        return 0;
    }
    else if((mythread->pth_list_length + count) > mythread->pth_list_size) {
        if(status)
            *status = MyHTML_STATUS_THREAD_ERROR_NO_SLOTS;
        
        return 0;
    }
    
    if(count == 0)
        count = 1;
    
    mythread->batch_first_id = 0;
    mythread->batch_count    = count;
    
    size_t start = mythread->pth_list_length;
    *status = MyHTML_STATUS_OK;
    
    mybool_t init_first = myfalse;
    
    for (size_t i = 0; i < count; i++) {
        
        mythread_id_t curr_id = _myhread_create_stream_raw(mythread, func, mythread_function_batch, status, count);
        
        if(init_first == myfalse) {
            mythread->batch_first_id = curr_id;
            init_first = mytrue;
        }
        
        if(*status)
        {
            for (size_t n = start; n < (start + i); n++)
            {
                mythread_list_t *thr = &mythread->pth_list[n];
                
                pthread_cancel(thr->pth);
                
                sem_post(thr->data.sem);
                sem_close(thr->data.sem);
                
                if(thr->data.sem_name)
                    free(thr->data.sem_name);
            }
            
            mythread->batch_first_id = 0;
            mythread->batch_count    = 0;
            
            break;
        }
        else {
            mythread->pth_list[curr_id].data.use = i;
        }
    }
    
    return mythread->batch_first_id;
}

// mythread queue functions
mythread_queue_t * mythread_queue_create(size_t size, myhtml_status_t *status)
{
    if(status)
        *status = MyHTML_STATUS_OK;
    
    if(size < 4096)
        size = 4096;
    
    mythread_queue_t* queue = (mythread_queue_t*)malloc(sizeof(mythread_queue_t));
    
    if(queue == NULL) {
        if(status)
            *status = MyHTML_STATUS_THREAD_ERROR_QUEUE_MALLOC;
        return NULL;
    }
    
    queue->nodes_pos_size = 512;
    queue->nodes_size     = size;
    queue->nodes          = (mythread_queue_node_t**)calloc(queue->nodes_pos_size, sizeof(mythread_queue_node_t*));
    
    if(queue->nodes == NULL) {
        free(queue);
        
        if(status)
            *status = MyHTML_STATUS_THREAD_ERROR_QUEUE_NODES_MALLOC;
        return NULL;
    }
    
    mythread_queue_clean(queue);
    
    queue->nodes[queue->nodes_pos] = (mythread_queue_node_t*)malloc(sizeof(mythread_queue_node_t) * queue->nodes_size);
    
    if(queue->nodes[queue->nodes_pos] == NULL) {
        free(queue->nodes);
        free(queue);
        
        if(status)
            *status = MyHTML_STATUS_THREAD_ERROR_QUEUE_NODE_MALLOC;
        return NULL;
    }
    
    return queue;
}

void mythread_queue_clean(mythread_queue_t* queue)
{
    queue->nodes_length = 0;
    queue->nodes_pos    = 0;
    queue->nodes_root   = 0;
    queue->nodes_uses   = 0;
    
    if(queue->nodes[queue->nodes_pos])
        mythread_queue_node_clean(&queue->nodes[queue->nodes_pos][queue->nodes_length]);
}

mythread_queue_t * mythread_queue_destroy(mythread_queue_t* queue)
{
    if(queue == NULL)
        return NULL;
    
    if(queue->nodes) {
        for (size_t i = 0; i <= queue->nodes_pos; i++) {
            free(queue->nodes[i]);
        }
        
        free(queue->nodes);
    }
    
    free(queue);
    
    return NULL;
}

void mythread_queue_node_clean(mythread_queue_node_t* qnode)
{
    qnode->tree           = NULL;
    qnode->text           = NULL;
    qnode->token          = NULL;
    qnode->begin          = 0;
    qnode->length         = 0;
}

mythread_queue_node_t * mythread_queue_get_prev_node(mythread_queue_t* queue)
{
    if(queue->nodes_length == 0)
    {
        if(queue->nodes_pos == 0)
            return NULL;
        
        return &queue->nodes[(queue->nodes_pos - 1)][(queue->nodes_size - 1)];
    }
    
    return &queue->nodes[queue->nodes_pos][(queue->nodes_length - 1)];
}

mythread_queue_node_t * mythread_queue_get_current_node(mythread_queue_t* queue)
{
    return &queue->nodes[queue->nodes_pos][queue->nodes_length];
}

mythread_queue_node_t * mythread_queue_node_malloc(mythread_queue_t* queue, const char* text, size_t begin, myhtml_status_t *status)
{
    queue->nodes_length++;
    
    if(queue->nodes_length >= queue->nodes_size)
    {
        queue->nodes_pos++;
        
        if(queue->nodes_pos >= queue->nodes_pos_size)
        {
            queue->nodes_pos_size <<= 1;
            mythread_queue_node_t** tmp = realloc(queue->nodes, sizeof(mythread_queue_node_t*) * queue->nodes_pos_size);
            
            if(tmp) {
                memset(&tmp[queue->nodes_pos], 0, sizeof(mythread_queue_node_t*) * (queue->nodes_pos_size - queue->nodes_pos));
                
                queue->nodes = tmp;
            }
            else {
                if(status)
                    *status = MyHTML_STATUS_THREAD_ERROR_QUEUE_NODES_MALLOC;
                
                return NULL;
            }
        }
        
        if(queue->nodes[queue->nodes_pos] == NULL) {
            queue->nodes[queue->nodes_pos] = (mythread_queue_node_t*)malloc(sizeof(mythread_queue_node_t) * queue->nodes_size);
            
            if(queue->nodes[queue->nodes_pos] == NULL) {
                if(status)
                    *status = MyHTML_STATUS_THREAD_ERROR_QUEUE_NODE_MALLOC;
                
                return NULL;
            }
        }
        
        queue->nodes_length = 0;
    }
    
    queue->nodes_uses++;
    
    mythread_queue_node_t *qnode = &queue->nodes[queue->nodes_pos][queue->nodes_length];
    
    qnode->text  = text;
    qnode->begin = begin;
    
    return qnode;
}

// mythread functions

void mythread_wait_all(mythread_t *mythread)
{
    const struct timespec tomeout = {0, 10000};
    
    for (size_t idx = mythread->pth_list_root; idx < mythread->pth_list_length; idx++) {
        //mythread_context_t *data = &mythread->pth_list[idx].data;
        
        while(mythread->pth_list[idx].data.use < mythread->queue->nodes_uses) {
            nanosleep(&tomeout, NULL);
        }
    }
}
//
//void mythread_batch_wait_all(mythread_t *mythread, mythread_id_t first_id)
//{
//    const struct timespec tomeout = {0, 10000};
//
//    for (size_t idx = first_id; idx < (mythread->batch_first_id + mythread->batch_count); idx++) {
//        while(mythread->pth_list[idx].data.use < mythread->queue->nodes_uses) {
//            nanosleep(&tomeout, NULL);
//        }
//    }
//}
//

void mythread_stream_quit_all(mythread_t *mythread)
{
    mythread->stream_opt = MyTHREAD_OPT_QUIT;
}

void mythread_batch_quit_all(mythread_t *mythread)
{
    mythread->batch_opt = MyTHREAD_OPT_QUIT;
}

void mythread_stream_pause_all(mythread_t *mythread)
{
    mythread->stream_opt = MyTHREAD_OPT_WAIT;
}

void mythread_batch_pause_all(mythread_t *mythread)
{
    mythread->batch_opt = MyTHREAD_OPT_WAIT;
}

void mythread_resume_all(mythread_t *mythread)
{
    if(mythread->stream_opt == MyTHREAD_OPT_UNDEF &&
       mythread->stream_opt == MyTHREAD_OPT_UNDEF)
        return;
    
    mythread->stream_opt = MyTHREAD_OPT_UNDEF;
    mythread->batch_opt  = MyTHREAD_OPT_UNDEF;
    
    for (size_t idx = mythread->pth_list_root; idx < mythread->pth_list_length; idx++) {
        sem_post(mythread->pth_list[idx].data.sem);
    }
}

void mythread_function_stream(void *arg)
{
    mythread_context_t *ctx = (mythread_context_t*)arg;
    mythread_t * mythread = ctx->mythread;
    mythread_queue_t *queue = mythread->queue;
    
    const struct timespec tomeout = {0, 10000};
    
    sem_wait(ctx->sem);
    
    do {
        while (ctx->use >= queue->nodes_uses) {
            if(mythread->stream_opt & MyTHREAD_OPT_WAIT) {
                if(ctx->use >= queue->nodes_uses) {
                    ctx->opt = MyTHREAD_OPT_WAIT;
                    sem_wait(ctx->sem);
                    ctx->opt = MyTHREAD_OPT_UNDEF;
                }
            }
            else if(mythread->stream_opt & MyTHREAD_OPT_QUIT) {
                if(ctx->use >= queue->nodes_uses) {
                    sem_close(ctx->sem);
                    ctx->opt = MyTHREAD_OPT_QUIT;
                    return;
                }
            }
            
            nanosleep(&tomeout, NULL);
        }
        
        size_t pos = ctx->use / queue->nodes_size;
        size_t len = ctx->use % queue->nodes_size;
        
        mythread_queue_node_t *qnode = &queue->nodes[pos][len];
        
        ctx->func(ctx->id, qnode);
        ctx->use++;
    }
    while (1);
}

void mythread_function_batch(void *arg)
{
    mythread_context_t *ctx = (mythread_context_t*)arg;
    mythread_t * mythread = ctx->mythread;
    mythread_queue_t *queue = mythread->queue;
    
    const struct timespec tomeout = {0, 10000};
    
    sem_wait(ctx->sem);
    
    do {
        while (ctx->use >= queue->nodes_uses) {
            if(mythread->batch_opt & MyTHREAD_OPT_WAIT) {
                if(ctx->use >= queue->nodes_uses) {
                    ctx->opt = MyTHREAD_OPT_WAIT;
                    sem_wait(ctx->sem);
                    ctx->opt = MyTHREAD_OPT_UNDEF;
                }
            }
            else if(mythread->batch_opt & MyTHREAD_OPT_QUIT) {
                if(ctx->use >= queue->nodes_uses) {
                    sem_close(ctx->sem);
                    ctx->opt = MyTHREAD_OPT_QUIT;
                    return;
                }
            }
            
            nanosleep(&tomeout, NULL);
        }
        
        size_t pos = ctx->use / queue->nodes_size;
        size_t len = ctx->use % queue->nodes_size;
        
        mythread_queue_node_t *qnode = &queue->nodes[pos][len];
        
        ctx->func(ctx->id, qnode);
        ctx->use += ctx->t_count;
    }
    while (1);
}


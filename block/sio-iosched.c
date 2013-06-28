 * Simple IO scheduler
  * Based on Noop, Deadline and V(R) IO schedulers.
  *
- * Copyright (C) 2010 Miguel Boton <mboton@gmail.com>
+ * Copyright (C) 2012 Miguel Boton <mboton@gmail.com>
  *
  *
  * This algorithm does not do any kind of sorting, as it is aimed for
@@ -18,30 +18,33 @@
 #include <linux/bio.h>
 #include <linux/module.h>
 #include <linux/init.h>
-#include <linux/slab.h>
+#include <linux/version.h>
 
-enum {
-  ASYNC,
-  SYNC,
-};
+enum { ASYNC, SYNC };
 
 /* Tunables */
-static const int sync_expire = HZ / 2;  /* max time before a sync is submitted. */
-static const int async_expire = 5 * HZ;  /* ditto for async, these limits are SOFT! */
-static const int fifo_batch = 16;  /* # of sequential requests treated as one
-             by the above parameters. For throughput. */
+static const int sync_read_expire  = HZ / 2;  /* max time before a sync read is submitted. */
+static const int sync_write_expire = 2 * HZ;  /* max time before a sync write is submitted. */
+
+static const int async_read_expire  =  4 * HZ;  /* ditto for async, these limits are SOFT! */
+static const int async_write_expire = 16 * HZ;  /* ditto for async, these limits are SOFT! */
+
+static const int writes_starved = 4;    /* max times reads can starve a write */
+static const int fifo_batch     = 1;    /* # of sequential requests treated as one by the above parameters. For throughput. */
 
 /* Elevator data */
 struct sio_data {
   /* Request queues */
-  struct list_head fifo_list[2];
+  struct list_head fifo_list[2][2];
 
   /* Attributes */
   unsigned int batched;
+  unsigned int starved;
 
   /* Settings */
-  int fifo_expire[2];
+  int fifo_expire[2][2];
   int fifo_batch;
+  int writes_starved;
 };
 
 static void
@@ -68,35 +71,39 @@ struct sio_data {
 {
   struct sio_data *sd = q->elevator->elevator_data;
   const int sync = rq_is_sync(rq);
+  const int data_dir = rq_data_dir(rq);
 
   /*
    * Add request to the proper fifo list and set its
    * expire time.
    */
-  rq_set_fifo_time(rq, jiffies + sd->fifo_expire[sync]);
-  list_add_tail(&rq->queuelist, &sd->fifo_list[sync]);
+  rq_set_fifo_time(rq, jiffies + sd->fifo_expire[sync][data_dir]);
+  list_add_tail(&rq->queuelist, &sd->fifo_list[sync][data_dir]);
 }
 
+#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,38)
 static int
 sio_queue_empty(struct request_queue *q)
 {
   struct sio_data *sd = q->elevator->elevator_data;
 
   /* Check if fifo lists are empty */
-  return list_empty(&sd->fifo_list[SYNC]) &&
-         list_empty(&sd->fifo_list[ASYNC]);
+  return list_empty(&sd->fifo_list[SYNC][READ]) && list_empty(&sd->fifo_list[SYNC][WRITE]) &&
+         list_empty(&sd->fifo_list[ASYNC][READ]) && list_empty(&sd->fifo_list[ASYNC][WRITE]);
 }
+#endif
 
 static struct request *
-sio_expired_request(struct sio_data *sd, int sync)
+sio_expired_request(struct sio_data *sd, int sync, int data_dir)
 {
+  struct list_head *list = &sd->fifo_list[sync][data_dir];
   struct request *rq;
 
-  if (list_empty(&sd->fifo_list[sync]))
+  if (list_empty(list))
     return NULL;
 
   /* Retrieve request */
-  rq = rq_entry_fifo(sd->fifo_list[sync].next);
+  rq = rq_entry_fifo(list->next);
 
   /* Request has expired */
   if (time_after(jiffies, rq_fifo_time(rq)))
@@ -108,34 +115,50 @@ struct sio_data {
 static struct request *
 sio_choose_expired_request(struct sio_data *sd)
 {
-  struct request *sync = sio_expired_request(sd, SYNC);
-  struct request *async = sio_expired_request(sd, ASYNC);
+  struct request *rq;
 
   /*
-   * Check expired requests. Asynchronous requests have
-   * priority over synchronous.
+   * Check expired requests.
+   * Asynchronous requests have priority over synchronous.
+   * Write requests have priority over read.
    */
-  if (sync && async)
-    return async;
-  if (sync)
-    return sync;
+  rq = sio_expired_request(sd, ASYNC, WRITE);
+  if (rq)
+    return rq;
+  rq = sio_expired_request(sd, ASYNC, READ);
+  if (rq)
+    return rq;
 
-  return async;
+  rq = sio_expired_request(sd, SYNC, WRITE);
+  if (rq)
+    return rq;
+  rq = sio_expired_request(sd, SYNC, READ);
+  if (rq)
+    return rq;
 
+  return NULL;
 }
 
 static struct request *
-sio_choose_request(struct sio_data *sd)
+sio_choose_request(struct sio_data *sd, int data_dir)
 {
+  struct list_head *sync = sd->fifo_list[SYNC];
+  struct list_head *async = sd->fifo_list[ASYNC];
+
   /*
    * Retrieve request from available fifo list.
    * Synchronous requests have priority over asynchronous.
+   * Read requests have priority over write.
    */
-  if (!list_empty(&sd->fifo_list[SYNC]))
-    return rq_entry_fifo(sd->fifo_list[SYNC].next);
+  if (!list_empty(&sync[data_dir]))
+    return rq_entry_fifo(sync[data_dir].next);
+  if (!list_empty(&async[data_dir]))
+    return rq_entry_fifo(async[data_dir].next);
 
-  if (!list_empty(&sd->fifo_list[ASYNC]))
-    return rq_entry_fifo(sd->fifo_list[ASYNC].next);
+  if (!list_empty(&sync[!data_dir]))
+    return rq_entry_fifo(sync[!data_dir].next);
+  if (!list_empty(&async[!data_dir]))
+    return rq_entry_fifo(async[!data_dir].next);
 
   return NULL;
 }
@@ -151,6 +174,11 @@ struct sio_data {
   elv_dispatch_add_tail(rq->q, rq);
 
   sd->batched++;
+
+  if (rq_data_dir(rq))
+    sd->starved = 0;
+  else
+    sd->starved++;
 }
 
 static int
@@ -158,6 +186,7 @@ struct sio_data {
 {
   struct sio_data *sd = q->elevator->elevator_data;
   struct request *rq = NULL;
+  int data_dir = READ;
 
   /*
    * Retrieve any expired request after a batch of
@@ -170,7 +199,10 @@ struct sio_data {
 
   /* Retrieve request */
   if (!rq) {
-    rq = sio_choose_request(sd);
+    if (sd->starved > sd->writes_starved)
+      data_dir = WRITE;
+
+    rq = sio_choose_request(sd, data_dir);
     if (!rq)
       return 0;
   }
@@ -186,8 +218,9 @@ struct sio_data {
 {
   struct sio_data *sd = q->elevator->elevator_data;
   const int sync = rq_is_sync(rq);
+  const int data_dir = rq_data_dir(rq);
 
-  if (rq->queuelist.prev == &sd->fifo_list[sync])
+  if (rq->queuelist.prev == &sd->fifo_list[sync][data_dir])
     return NULL;
 
   /* Return former request */
@@ -199,8 +232,9 @@ struct sio_data {
 {
   struct sio_data *sd = q->elevator->elevator_data;
   const int sync = rq_is_sync(rq);
+  const int data_dir = rq_data_dir(rq);
 
-  if (rq->queuelist.next == &sd->fifo_list[sync])
+  if (rq->queuelist.next == &sd->fifo_list[sync][data_dir])
     return NULL;
 
   /* Return latter request */
@@ -218,13 +252,17 @@ struct sio_data {
     return NULL;
 
   /* Initialize fifo lists */
-  INIT_LIST_HEAD(&sd->fifo_list[SYNC]);
-  INIT_LIST_HEAD(&sd->fifo_list[ASYNC]);
+  INIT_LIST_HEAD(&sd->fifo_list[SYNC][READ]);
+  INIT_LIST_HEAD(&sd->fifo_list[SYNC][WRITE]);
+  INIT_LIST_HEAD(&sd->fifo_list[ASYNC][READ]);
+  INIT_LIST_HEAD(&sd->fifo_list[ASYNC][WRITE]);
 
   /* Initialize data */
   sd->batched = 0;
-  sd->fifo_expire[SYNC] = sync_expire;
-  sd->fifo_expire[ASYNC] = async_expire;
+  sd->fifo_expire[SYNC][READ] = sync_read_expire;
+  sd->fifo_expire[SYNC][WRITE] = sync_write_expire;
+  sd->fifo_expire[ASYNC][READ] = async_read_expire;
+  sd->fifo_expire[ASYNC][WRITE] = async_write_expire;
   sd->fifo_batch = fifo_batch;
 
   return sd;
@@ -235,8 +273,10 @@ struct sio_data {
 {
   struct sio_data *sd = e->elevator_data;
 
-  BUG_ON(!list_empty(&sd->fifo_list[SYNC]));
-  BUG_ON(!list_empty(&sd->fifo_list[ASYNC]));
+  BUG_ON(!list_empty(&sd->fifo_list[SYNC][READ]));
+  BUG_ON(!list_empty(&sd->fifo_list[SYNC][WRITE]));
+  BUG_ON(!list_empty(&sd->fifo_list[ASYNC][READ]));
+  BUG_ON(!list_empty(&sd->fifo_list[ASYNC][WRITE]));
 
   /* Free structure */
   kfree(sd);
@@ -270,9 +310,12 @@ static ssize_t __FUNC(struct elevator_queue *e, char *page)    \
     __data = jiffies_to_msecs(__data);      \
   return sio_var_show(__data, (page));      \
 }
-SHOW_FUNCTION(sio_sync_expire_show, sd->fifo_expire[SYNC], 1);
-SHOW_FUNCTION(sio_async_expire_show, sd->fifo_expire[ASYNC], 1);
+SHOW_FUNCTION(sio_sync_read_expire_show, sd->fifo_expire[SYNC][READ], 1);
+SHOW_FUNCTION(sio_sync_write_expire_show, sd->fifo_expire[SYNC][WRITE], 1);
+SHOW_FUNCTION(sio_async_read_expire_show, sd->fifo_expire[ASYNC][READ], 1);
+SHOW_FUNCTION(sio_async_write_expire_show, sd->fifo_expire[ASYNC][WRITE], 1);
 SHOW_FUNCTION(sio_fifo_batch_show, sd->fifo_batch, 0);
+SHOW_FUNCTION(sio_writes_starved_show, sd->writes_starved, 0);
 #undef SHOW_FUNCTION
 
 #define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)      \
@@ -291,9 +334,12 @@ static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)
     *(__PTR) = __data;          \
   return ret;              \
 }
-STORE_FUNCTION(sio_sync_expire_store, &sd->fifo_expire[SYNC], 0, INT_MAX, 1);
-STORE_FUNCTION(sio_async_expire_store, &sd->fifo_expire[ASYNC], 0, INT_MAX, 1);
+STORE_FUNCTION(sio_sync_read_expire_store, &sd->fifo_expire[SYNC][READ], 0, INT_MAX, 1);
+STORE_FUNCTION(sio_sync_write_expire_store, &sd->fifo_expire[SYNC][WRITE], 0, INT_MAX, 1);
+STORE_FUNCTION(sio_async_read_expire_store, &sd->fifo_expire[ASYNC][READ], 0, INT_MAX, 1);
+STORE_FUNCTION(sio_async_write_expire_store, &sd->fifo_expire[ASYNC][WRITE], 0, INT_MAX, 1);
 STORE_FUNCTION(sio_fifo_batch_store, &sd->fifo_batch, 0, INT_MAX, 0);
+STORE_FUNCTION(sio_writes_starved_store, &sd->writes_starved, 0, INT_MAX, 0);
 #undef STORE_FUNCTION
 
 #define DD_ATTR(name) \
@@ -301,9 +347,12 @@ static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)
               sio_##name##_store)
 
 static struct elv_fs_entry sio_attrs[] = {
-  DD_ATTR(sync_expire),
-  DD_ATTR(async_expire),
+  DD_ATTR(sync_read_expire),
+  DD_ATTR(sync_write_expire),
+  DD_ATTR(async_read_expire),
+  DD_ATTR(async_write_expire),
   DD_ATTR(fifo_batch),
+  DD_ATTR(writes_starved),
   __ATTR_NULL
 };
 
@@ -312,7 +361,9 @@ static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)
     .elevator_merge_req_fn    = sio_merged_requests,
     .elevator_dispatch_fn    = sio_dispatch_requests,
     .elevator_add_req_fn    = sio_add_request,
+#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,38)
     .elevator_queue_empty_fn  = sio_queue_empty,
+#endif
     .elevator_former_req_fn    = sio_former_request,
     .elevator_latter_req_fn    = sio_latter_request,
     .elevator_init_fn    = sio_init_queue,
@@ -344,7 +395,5 @@ static void __exit sio_exit(void)
 MODULE_AUTHOR("Miguel Boton");
 MODULE_LICENSE("GPL");
 MODULE_DESCRIPTION("Simple IO scheduler");
-
-
-
+MODULE_VERSION("0.2");

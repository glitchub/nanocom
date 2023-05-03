// byte-queue primitives

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "queue.h"

// Add count bytes of data to specified queue, which is expanded as needed to fit
void putq(queue *q, void *data, int count)
{
    if (q->size <= q->count + count)
    {
        if (!q->size) q->size = 1024;               // first time init
        while (q->size <= q->count + count) q->size *= 2;
        q->data = realloc(q->data, q->size);        // just malloc() if q->data == NULL
        if (!q->data) abort();                      // abort on OOM
    }

    for (int i = 0; i < count; i++, q->count++)
        q->data[(q->head + q->count) % q->size] = *(unsigned char *)(data + i);
}

// Point *data at a monolithic chunk of queue data and return its length, or 0 if queue is empty.
// Caller must call delq() once the getq data has been consumed.
int getq(queue *q, void **data)
{
    if (!q->count) return 0;
    *data = q->data + q->head;
    int len = q->size - q->head;
    if (len > q->count) len = q->count;
    return len;
}

// Remove specified number of characters from queue, after using getq.
// If count < 0, delete everything.
void delq(queue *q, int count)
{
    if (!count) return;
    if (count < 0 || count >= q->count)
    {
        q->count = 0;
        q->head = 0;
        return;
    }
    q->count -= count;
    q->head = (q->head + count) % q->size;
}

// Read up to 256 bytes from file descriptor to queue, return total read or < 0 if error.
// File descriptor must be non-blocking.
int enqueue(queue *q, int fd)
{
    unsigned char buffer[256];
    int r = read(fd, &buffer, sizeof buffer);
    if (r > 0) putq(q, buffer, r);
    return r;
}

// Write bytes from queue to file descriptor, return total written or < 0 if error.
// File descriptor must be non-blocking.
int dequeue(queue *q, int fd)
{
    void *p;
    int n = getq(q, &p);
    if (!n) return 0;
    int r = write(fd, p, n);
    if (r > 0) delq(q, r);
    return r;
}

// Wipe queue and free malloc'd buffer
void freeq(queue *q)
{
    delq(q, -1);
    free(q->data);  // OK if NULL
    q->data = NULL;
}

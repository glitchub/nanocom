// Byte queue primitives

typedef struct
{
    int head,               // oldest byte is at data+head
        count,              // number of queued bytes
        size;               // size of data allocation
    unsigned char *data;    // malloced data buffer
} queue;

// Add count bytes of data to specified queue, which is expanded as needed to fit.
void putq(queue *q, void *bytes, int count);

// Point *bytes at a monolithic chunk of queue data and return its length, or 0 if queue is empty.
// Caller must call delq() once the getq data has been consumed.
int getq(queue *q, void **bytes);

// Remove specified number of characters from queue, after using getq.
// If count < 0, delete everything.
void delq(queue *q, int count);

// Read up to 256 bytes from file descriptor to queue, return number of bytes read or < 0 if error.
// File description must be non-blocking.
int enqueue(queue *q, int fd);

// Write bytes from queue to file descriptor, return number of bytes written or < 0 if error.
// File descriptor must be non-blocking.
int dequeue(queue *q, int fd);

// Wipe queue and free malloc'd buffer
void freeq(queue *q);

// Number of bytes in queue
#define availq(q) ((q)->count)

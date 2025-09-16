/*
 * Sometimes I need something better that grafana or a %top like program,
 * and to test short spikes in CPU much faster, so this code will do it
 * at 100HZ, plus get a list of the 5 most CPU intensive processes.
 * I use gcc -std=c2x -O3 -Wall -Wextra cpu100.c -o cpu100 to compile it,
 * and to run it ./cpu100 &> some_log_file.
*/

#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>

// Global flag for clean shutdown on Ctrl+C
static volatile sig_atomic_t keep_running = 1;
static void on_sigint(int sig) { (void)sig; keep_running = 0; }

// Structs for storing CPU and process statistics
typedef struct { unsigned long long idle, total; } cpu_sample;
typedef struct { int pid; unsigned long long ticks; char comm[64]; } proc_sample;
typedef struct { int pid; char comm[64]; double pct; } proc_usage;

// Process cache to avoid constant memory reallocation
typedef struct {
    proc_sample *samples;  // Array of process samples
    int capacity;          // Current allocated capacity
    int count;             // Number of active processes
} proc_cache;

/**
 * Gets the number of CPUs in the system
 * Returns: CPU count from sysconf
 */
static int ncpu(void) {
    long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n < 1) { perror("sysconf"); exit(1); }
    return (int)n;
}

/**
 * Pins the monitoring process to the last CPU core
 * This reduces interference with other processes being monitored
 * @param n: Total number of CPUs in the system
 */
static void pin_to_last_cpu(int n) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(n - 1, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity"); exit(1);
    }
}

/**
 * Reads CPU statistics from /proc/stat efficiently
 * Uses persistent file descriptor and buffer to minimize syscall overhead
 * @param out: Array to store CPU samples (one per CPU core)
 * @param n: Number of CPUs to read
 */
static void read_proc_stat_optimized(cpu_sample *out, int n) {
    static int fd = -1;           // Persistent file descriptor
    static char *buf = NULL;      // Reusable buffer
    static size_t bufsize = 8192; // Buffer size for /proc/stat content
    
    // Initialize on first call
    if (fd == -1) {
        fd = open("/proc/stat", O_RDONLY);
        if (fd == -1) { perror("open /proc/stat"); exit(1); }
        buf = malloc(bufsize);
    }
    
    // Reset file position and read entire file at once
    lseek(fd, 0, SEEK_SET);
    ssize_t bytes = read(fd, buf, bufsize - 1);
    if (bytes <= 0) { perror("read /proc/stat"); exit(1); }
    buf[bytes] = '\0';
    
    // Parse CPU statistics line by line
    char *line = buf;
    char *next = strchr(line, '\n');
    if (next) { line = next + 1; } // Skip aggregate "cpu" line
    
    // Process each CPU core's statistics
    for (int i = 0; i < n; i++) {
        next = strchr(line, '\n');
        if (!next) { fprintf(stderr, "Unexpected EOF in /proc/stat\n"); exit(1); }
        
        // Parse the 10 CPU time fields:
        // user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice
        unsigned long long v[10] = {0};
        int m = sscanf(line,
            "cpu%*d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
            &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7],&v[8],&v[9]);
        
        if (m < 4) { fprintf(stderr, "Parse error in /proc/stat\n"); exit(1); }
        
        // Calculate idle time (idle + iowait) and total time
        unsigned long long idle = v[3] + v[4];
        unsigned long long total = 0;
        for (int k = 0; k < m; k++) total += v[k];
        out[i] = (cpu_sample){idle, total};
        
        line = next + 1;
    }
}

// Hash table for O(1) process ID lookups
#define HASH_SIZE 1024
typedef struct pid_node {
    int pid;               // Process ID
    int index;            // Index in the process array
    struct pid_node *next; // Collision chain
} pid_node;

/**
 * Simple hash function for process IDs
 * @param pid: Process ID to hash
 * Returns: Hash bucket index
 */
static unsigned int pid_hash(int pid) {
    return (unsigned int)pid % HASH_SIZE;
}

/**
 * Reads all process statistics from /proc/[pid]/stat files
 * Uses hash table for efficient PID lookups in subsequent comparisons
 * @param cache: Process cache to store results
 * @param hash_table: Hash table for O(1) PID lookups
 * Returns: Number of processes read
 */
static int read_processes_optimized(proc_cache *cache, pid_node **hash_table) {
    static DIR *d = NULL;      // Persistent directory handle
    static char buf[1024];      // Buffer for reading stat files
    
    // Open /proc directory on first call, rewind on subsequent calls
    if (!d) {
        d = opendir("/proc");
        if (!d) { perror("opendir"); exit(1); }
    } else {
        rewinddir(d);
    }
    
    // Clear previous hash table
    for (int i = 0; i < HASH_SIZE; i++) {
        pid_node *curr = hash_table[i];
        while (curr) {
            pid_node *next = curr->next;
            free(curr);
            curr = next;
        }
        hash_table[i] = NULL;
    }
    
    cache->count = 0;
    struct dirent *de;
    
    // Iterate through /proc entries
    while ((de = readdir(d))) {
        // Skip non-numeric entries (not process directories)
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        
        int pid = atoi(de->d_name);
        char path[256];
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        
        // Read process stat file
        int fd = open(path, O_RDONLY);
        if (fd == -1) continue;
        
        ssize_t bytes = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (bytes <= 0) continue;
        buf[bytes] = '\0';
        
        // Parse process name (comm) field - it's in parentheses
        char *comm_start = strchr(buf, '(');
        char *comm_end = strrchr(buf, ')');
        if (!comm_start || !comm_end || comm_end <= comm_start) continue;
        
        *comm_end = '\0';
        char *p = comm_end + 2; // Skip ") " to get to state field
        
        // Skip state and 10 more fields to reach utime (field 14)
        for (int i = 0; i < 11; i++) {
            p = strchr(p, ' ');
            if (!p) goto next_pid;
            p++;
        }
        
        // Parse user time and system time (fields 14 and 15)
        unsigned long long ut, st;
        if (sscanf(p, "%llu %llu", &ut, &st) != 2) goto next_pid;
        
        // Expand cache if needed
        if (cache->count >= cache->capacity) {
            cache->capacity *= 2;
            cache->samples = realloc(cache->samples, 
                                    cache->capacity * sizeof(proc_sample));
        }
        
        // Store process information
        int idx = cache->count;
        cache->samples[idx].pid = pid;
        cache->samples[idx].ticks = ut + st;  // Total CPU ticks used
        strncpy(cache->samples[idx].comm, comm_start + 1, 63);
        cache->samples[idx].comm[63] = '\0';
        
        // Add to hash table for fast lookup
        unsigned int h = pid_hash(pid);
        pid_node *node = malloc(sizeof(pid_node));
        node->pid = pid;
        node->index = idx;
        node->next = hash_table[h];
        hash_table[h] = node;
        
        cache->count++;
        
    next_pid:
        continue;
    }
    
    return cache->count;
}

/**
 * Partition function for quickselect algorithm
 * Partitions array around pivot for finding top K elements
 * @param arr: Array of process usage data
 * @param low: Start index
 * @param high: End index
 * Returns: Final position of pivot element
 */
static int partition(proc_usage *arr, int low, int high) {
    double pivot = arr[high].pct;
    int i = low - 1;
    
    // Move elements greater than pivot to the left
    for (int j = low; j < high; j++) {
        if (arr[j].pct > pivot) {
            i++;
            proc_usage temp = arr[i];
            arr[i] = arr[j];
            arr[j] = temp;
        }
    }
    
    // Place pivot in correct position
    proc_usage temp = arr[i + 1];
    arr[i + 1] = arr[high];
    arr[high] = temp;
    
    return i + 1;
}

/**
 * Finds and sorts only the top 5 processes by CPU usage
 * Uses quickselect for O(n) average time complexity
 * @param arr: Array of process usage data
 * @param n: Total number of processes
 */
static void quickselect_top5(proc_usage *arr, int n) {
    if (n <= 5) return;  // Already have 5 or fewer elements
    
    // Use quickselect to partition array so top 5 are at the beginning
    int left = 0, right = n - 1;
    while (left < right) {
        int pivot_idx = partition(arr, left, right);
        if (pivot_idx == 4) break;  // Top 5 are now in first 5 positions
        else if (pivot_idx < 4) left = pivot_idx + 1;
        else right = pivot_idx - 1;
    }
    
    // Sort just the top 5 elements
    for (int i = 0; i < 5 && i < n; i++) {
        for (int j = i + 1; j < 5 && j < n; j++) {
            if (arr[j].pct > arr[i].pct) {
                proc_usage temp = arr[i];
                arr[i] = arr[j];
                arr[j] = temp;
            }
        }
    }
}

/**
 * Calculates CPU usage for each process and prints top 5
 * Compares current and previous samples to determine CPU percentage
 * @param prev: Previous process cache
 * @param prev_hash: Hash table for previous processes
 * @param cur: Current process cache
 * @param dt_ticks: Total CPU ticks elapsed across all cores
 */
static void print_top5_optimized(proc_cache *prev, pid_node **prev_hash,
                                proc_cache *cur, unsigned long long dt_ticks) {
    static proc_usage *arr = NULL;  // Reusable array for calculations
    static int arr_capacity = 0;
    
    // Ensure array is large enough
    if (arr_capacity < cur->count) {
        arr_capacity = cur->count;
        arr = realloc(arr, arr_capacity * sizeof(proc_usage));
    }
    
    int n = 0;
    // Calculate CPU usage for each current process
    for (int i = 0; i < cur->count; i++) {
        int pid = cur->samples[i].pid;
        unsigned int h = pid_hash(pid);
        
        // Look up this process in previous sample using hash table
        pid_node *node = prev_hash[h];
        while (node) {
            if (node->pid == pid) {
                int prev_idx = node->index;
                if (prev_idx < prev->count) {
                    // Calculate CPU usage percentage
                    unsigned long long d = cur->samples[i].ticks - 
                                         prev->samples[prev_idx].ticks;
                    double pct = dt_ticks ? 100.0 * (double)d / (double)dt_ticks : 0.0;
                    
                    arr[n].pid = pid;
                    strncpy(arr[n].comm, cur->samples[i].comm, 63);
                    arr[n].comm[63] = '\0';
                    arr[n].pct = pct;
                    n++;
                }
                break;
            }
            node = node->next;
        }
    }
    
    // Find and sort top 5 processes
    quickselect_top5(arr, n);
    
    // Print top 5 processes
    int top = n < 5 ? n : 5;
    for (int i = 0; i < top; i++) {
        printf("    pid=%d %-20s %.1f%%\n", arr[i].pid, arr[i].comm, arr[i].pct);
    }
}

/**
 * Creates timestamp string with centisecond precision
 * Format: HH:MM:SS:CC where CC is centiseconds (1/100 second)
 * @param buf: Buffer to store timestamp string
 * @param sz: Size of buffer
 */
static void timestamp_centis(char *buf, size_t sz) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int centi = (int)(ts.tv_nsec / 10000000L);  // Convert nanoseconds to centiseconds
    if (centi > 99) centi = 99;
    snprintf(buf, sz, "%02d:%02d:%02d:%02d", 
             tm.tm_hour, tm.tm_min, tm.tm_sec, centi);
}

/**
 * Main monitoring loop
 * Samples CPU and process statistics at 100Hz (every 10ms)
 * Prints per-CPU usage and top 5 processes by CPU consumption
 */
int main(void) {
    // Set up signal handler for clean shutdown
    signal(SIGINT, on_sigint);
    
    // Initialize CPU monitoring
    int n = ncpu();
    pin_to_last_cpu(n);  // Run monitor on last CPU to minimize interference
    
    // Allocate CPU sample buffers (double buffering)
    cpu_sample *prevc = calloc(n, sizeof *prevc);
    cpu_sample *curc = calloc(n, sizeof *curc);
    
    // Initialize process caches with initial capacity
    proc_cache prev_cache = {NULL, 1024, 0};
    proc_cache cur_cache = {NULL, 1024, 0};
    prev_cache.samples = malloc(prev_cache.capacity * sizeof(proc_sample));
    cur_cache.samples = malloc(cur_cache.capacity * sizeof(proc_sample));
    
    // Hash tables for O(1) process lookups
    pid_node **prev_hash = calloc(HASH_SIZE, sizeof(pid_node*));
    pid_node **cur_hash = calloc(HASH_SIZE, sizeof(pid_node*));
    
    // Read initial samples
    read_proc_stat_optimized(prevc, n);
    read_processes_optimized(&prev_cache, prev_hash);
    
    // Sampling interval: 10ms = 100 samples per second
    struct timespec interval = {0, 10 * 1000 * 1000}; // 10ms in nanoseconds
    
    // Print header
    printf("HH:MM:SS:UU");
    for (int i = 0; i < n; i++) printf("\tcpu_%d", i);
    putchar('\n');
    
    // Main monitoring loop
    while (keep_running) {
        nanosleep(&interval, NULL);
        
        // Read current CPU and process statistics
        read_proc_stat_optimized(curc, n);
        read_processes_optimized(&cur_cache, cur_hash);
        
        // Print timestamp
        char tbuf[32];
        timestamp_centis(tbuf, sizeof tbuf);
        printf("%s", tbuf);
        
        // Calculate and print per-CPU usage
        for (int i = 0; i < n; i++) {
            unsigned long long dt = curc[i].total - prevc[i].total;  // Total ticks
            unsigned long long di = curc[i].idle - prevc[i].idle;    // Idle ticks
            double usage = dt ? 100.0 * (dt - di) / (double)dt : 0.0;
            printf("\t%2.0f%%", usage);
        }
        putchar('\n');
        
        // Calculate total system ticks for process percentage calculation
        unsigned long long dt_ticks = 0;
        for (int i = 0; i < n; i++) {
            dt_ticks += curc[i].total - prevc[i].total;
        }
        
        // Print top 5 processes by CPU usage
        print_top5_optimized(&prev_cache, prev_hash, &cur_cache, dt_ticks);
        
        // Swap buffers for next iteration (double buffering technique)
        cpu_sample *tmpc = prevc; prevc = curc; curc = tmpc;
        
        // Swap process caches and hash tables
        proc_cache tmp_cache = prev_cache;
        prev_cache = cur_cache;
        cur_cache = tmp_cache;
        
        pid_node **tmp_hash = prev_hash;
        prev_hash = cur_hash;
        cur_hash = tmp_hash;
        
        fflush(stdout);  // Ensure output is displayed immediately
    }
    
    // Clean up allocated memory
    free(prevc);
    free(curc);
    free(prev_cache.samples);
    free(cur_cache.samples);
    
    // Clean up hash tables
    for (int i = 0; i < HASH_SIZE; i++) {
        pid_node *curr = prev_hash[i];
        while (curr) {
            pid_node *next = curr->next;
            free(curr);
            curr = next;
        }
        curr = cur_hash[i];
        while (curr) {
            pid_node *next = curr->next;
            free(curr);
            curr = next;
        }
    }
    
    free(prev_hash);
    free(cur_hash);
    
    return 0;
}

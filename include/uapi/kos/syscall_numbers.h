#ifndef KOS_UAPI_SYSCALL_NUMBERS_H
#define KOS_UAPI_SYSCALL_NUMBERS_H

/* Standard POSIX-aligned Syscall Numbers for KOS */
#define SYS_exit        1
#define SYS_fork        2
#define SYS_read        3
#define SYS_write       4
#define SYS_open        5
#define SYS_close       6
#define SYS_waitpid     7
#define SYS_execve      8
#define SYS_stat        9
#define SYS_fstat       10
#define SYS_lseek       11
#define SYS_mmap        12
#define SYS_munmap      13
#define SYS_mprotect    14
#define SYS_getpid      15
#define SYS_getppid     16
#define SYS_yield       17
#define SYS_sleep       18
#define SYS_unlink      19
#define SYS_socket      20
#define SYS_connect     21
#define SYS_accept      22
#define SYS_sendto      23
#define SYS_recvfrom    24
#define SYS_bind        25
#define SYS_listen      26
#define SYS_poll        27
#define SYS_setsockopt  28
#define SYS_getsockopt  29
#define SYS_gettime     30
#define SYS_sysinfo     31
#define SYS_mkdir       32
#define SYS_getdents    33
#define SYS_rename      34
#define SYS_gui_display 35
#define SYS_gui_input   36
#define SYS_installer   37
#define SYS_system_info 38
#define SYS_gui_control 39
#define SYS_system_control 40

#define KOS_MAX_SYSCALL_NR 64

/* Standard POSIX Negative Error Codes */
#define EPERM           1   /* Operation not permitted */
#define ENOENT          2   /* No such file or directory */
#define ESRCH           3   /* No such process */
#define EINTR           4   /* Interrupted system call */
#define EIO             5   /* I/O error */
#define ENXIO           6   /* No such device or address */
#define E2BIG           7   /* Argument list too long */
#define ENOEXEC         8   /* Exec format error */
#define EBADF           9   /* Bad file descriptor */
#define ECHILD          10  /* No child processes */
#define EAGAIN          11  /* Resource temporarily unavailable */
#define ENOMEM          12  /* Out of memory */
#define EACCES          13  /* Permission denied */
#define EFAULT          14  /* Bad address */
#define EBUSY           16  /* Device or resource busy */
#define EEXIST          17  /* File exists */
#define ENODEV          19  /* No such device */
#define ENOTDIR         20  /* Not a directory */
#define EISDIR          21  /* Is a directory */
#define EINVAL          22  /* Invalid argument */
#define ENFILE          23  /* File table overflow */
#define EMFILE          24  /* Too many open files */
#define ENOSPC          28  /* No space left on device */
#define ESPIPE          29  /* Illegal seek */
#define EPIPE           32  /* Broken pipe */
#define EAFNOSUPPORT    97  /* Address family not supported */
#define ENOSYS          38  /* Function not implemented */
#define ENOTEMPTY       39  /* Directory not empty */
#define ENAMETOOLONG    36  /* File name too long */
#define EOVERFLOW       75  /* Value too large for data type */
#define ETIMEDOUT       110 /* Connection timed out */
#define ECONNREFUSED    111 /* Connection refused */

/* open(2) flags. O_RDONLY is deliberately zero, matching POSIX. */
#define KOS_O_RDONLY    0x0000U
#define KOS_O_WRONLY    0x0001U
#define KOS_O_RDWR      0x0002U
#define KOS_O_ACCMODE   0x0003U
#define KOS_O_CREAT     0x0040U
#define KOS_O_EXCL      0x0080U
#define KOS_O_TRUNC     0x0200U
#define KOS_O_APPEND    0x0400U
#define KOS_O_DIRECTORY 0x10000U

#define KOS_SEEK_SET    0U
#define KOS_SEEK_CUR    1U
#define KOS_SEEK_END    2U

enum kos_stat_type {
    KOS_STAT_FILE = 1,
    KOS_STAT_DIRECTORY = 2,
};

struct kos_stat {
    uint64_t size;
    uint32_t type;
    uint32_t attributes;
};

#define KOS_DIRENT_NAME_MAX 256

/* Fixed-size records keep getdents parsing simple for the first user ABI. */
struct kos_dirent {
    uint64_t size;
    uint32_t type;
    uint32_t name_length;
    char name[KOS_DIRENT_NAME_MAX];
};

/* poll(2) ABI. Socket descriptors are opaque KOS handles. */
#define KOS_POLLIN      0x0001
#define KOS_POLLPRI     0x0002
#define KOS_POLLOUT     0x0004
#define KOS_POLLERR     0x0008
#define KOS_POLLHUP     0x0010
#define KOS_POLLNVAL    0x0020

struct kos_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

#endif /* KOS_UAPI_SYSCALL_NUMBERS_H */

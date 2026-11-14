/* libwayland's configuration for Renga (meson writes this file in a
   regular build of the library). memfd_create() is libgnu's. */
#define PACKAGE "wayland"
#define PACKAGE_VERSION "1.21.90"
#define HAVE_ACCEPT4 1
#define HAVE_MEMFD_CREATE 1
#define HAVE_STRNDUP 1
#define HAVE_POSIX_FALLOCATE 1
#define HAVE_MKOSTEMP 1
#define HAVE_XUCRED_CR_PID 0
#define HAVE_BROKEN_MSG_CMSG_CLOEXEC 0

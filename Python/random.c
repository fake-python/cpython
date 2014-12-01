#include "Python.h"
#ifdef MS_WINDOWS
#include <windows.h>
#else
#include <fcntl.h>
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#endif

#ifdef Py_DEBUG
int _Py_HashSecret_Initialized = 0;
#else
static int _Py_HashSecret_Initialized = 0;
#endif

#ifdef MS_WINDOWS
/* This handle is never explicitly released. Instead, the operating
   system will release it when the process terminates. */
static HCRYPTPROV hCryptProv = 0;

static int
win32_urandom_init(int raise)
{
    /* Acquire context */
    if (!CryptAcquireContext(&hCryptProv, NULL, NULL,
                             PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        goto error;

    return 0;

error:
    if (raise)
        PyErr_SetFromWindowsErr(0);
    else
        Py_FatalError("Failed to initialize Windows random API (CryptoGen)");
    return -1;
}

/* Fill buffer with size pseudo-random bytes generated by the Windows CryptoGen
   API. Return 0 on success, or -1 on error. */
static int
win32_urandom(unsigned char *buffer, Py_ssize_t size, int raise)
{
    Py_ssize_t chunk;

    if (hCryptProv == 0)
    {
        if (win32_urandom_init(raise) == -1)
            return -1;
    }

    while (size > 0)
    {
        chunk = size > INT_MAX ? INT_MAX : size;
        if (!CryptGenRandom(hCryptProv, (DWORD)chunk, buffer))
        {
            /* CryptGenRandom() failed */
            if (raise)
                PyErr_SetFromWindowsErr(0);
            else
                Py_FatalError("Failed to initialized the randomized hash "
                        "secret using CryptoGen)");
            return -1;
        }
        buffer += chunk;
        size -= chunk;
    }
    return 0;
}
#endif /* MS_WINDOWS */


#ifndef MS_WINDOWS
static struct {
    int fd;
    dev_t st_dev;
    ino_t st_ino;
} urandom_cache = { -1 };

/* Read size bytes from /dev/urandom into buffer.
   Call Py_FatalError() on error. */
static void
dev_urandom_noraise(unsigned char *buffer, Py_ssize_t size)
{
    int fd;
    Py_ssize_t n;

    assert (0 < size);

    fd = _Py_open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        Py_FatalError("Failed to open /dev/urandom");

    while (0 < size)
    {
        do {
            n = read(fd, buffer, (size_t)size);
        } while (n < 0 && errno == EINTR);
        if (n <= 0)
        {
            /* stop on error or if read(size) returned 0 */
            Py_FatalError("Failed to read bytes from /dev/urandom");
            break;
        }
        buffer += n;
        size -= (Py_ssize_t)n;
    }
    close(fd);
}

/* Read size bytes from /dev/urandom into buffer.
   Return 0 on success, raise an exception and return -1 on error. */
static int
dev_urandom_python(char *buffer, Py_ssize_t size)
{
    int fd;
    Py_ssize_t n;
    struct stat st;

    if (size <= 0)
        return 0;

    if (urandom_cache.fd >= 0) {
        /* Does the fd point to the same thing as before? (issue #21207) */
        if (fstat(urandom_cache.fd, &st)
            || st.st_dev != urandom_cache.st_dev
            || st.st_ino != urandom_cache.st_ino) {
            /* Something changed: forget the cached fd (but don't close it,
               since it probably points to something important for some
               third-party code). */
            urandom_cache.fd = -1;
        }
    }
    if (urandom_cache.fd >= 0)
        fd = urandom_cache.fd;
    else {
        Py_BEGIN_ALLOW_THREADS
        fd = _Py_open("/dev/urandom", O_RDONLY);
        Py_END_ALLOW_THREADS
        if (fd < 0)
        {
            if (errno == ENOENT || errno == ENXIO ||
                errno == ENODEV || errno == EACCES)
                PyErr_SetString(PyExc_NotImplementedError,
                                "/dev/urandom (or equivalent) not found");
            else
                PyErr_SetFromErrno(PyExc_OSError);
            return -1;
        }
        if (urandom_cache.fd >= 0) {
            /* urandom_fd was initialized by another thread while we were
               not holding the GIL, keep it. */
            close(fd);
            fd = urandom_cache.fd;
        }
        else {
            if (fstat(fd, &st)) {
                PyErr_SetFromErrno(PyExc_OSError);
                close(fd);
                return -1;
            }
            else {
                urandom_cache.fd = fd;
                urandom_cache.st_dev = st.st_dev;
                urandom_cache.st_ino = st.st_ino;
            }
        }
    }

    Py_BEGIN_ALLOW_THREADS
    do {
        do {
            n = read(fd, buffer, (size_t)size);
        } while (n < 0 && errno == EINTR);
        if (n <= 0)
            break;
        buffer += n;
        size -= (Py_ssize_t)n;
    } while (0 < size);
    Py_END_ALLOW_THREADS

    if (n <= 0)
    {
        /* stop on error or if read(size) returned 0 */
        if (n < 0)
            PyErr_SetFromErrno(PyExc_OSError);
        else
            PyErr_Format(PyExc_RuntimeError,
                         "Failed to read %zi bytes from /dev/urandom",
                         size);
        return -1;
    }
    return 0;
}

static void
dev_urandom_close(void)
{
    if (urandom_cache.fd >= 0) {
        close(urandom_cache.fd);
        urandom_cache.fd = -1;
    }
}

#endif /* MS_WINDOWS */

/* Fill buffer with pseudo-random bytes generated by a linear congruent
   generator (LCG):

       x(n+1) = (x(n) * 214013 + 2531011) % 2^32

   Use bits 23..16 of x(n) to generate a byte. */
static void
lcg_urandom(unsigned int x0, unsigned char *buffer, size_t size)
{
    size_t index;
    unsigned int x;

    x = x0;
    for (index=0; index < size; index++) {
        x *= 214013;
        x += 2531011;
        /* modulo 2 ^ (8 * sizeof(int)) */
        buffer[index] = (x >> 16) & 0xff;
    }
}

/* Fill buffer with size pseudo-random bytes from the operating system random
   number generator (RNG). It is suitable for most cryptographic purposes
   except long living private keys for asymmetric encryption.

   Return 0 on success, raise an exception and return -1 on error. */
int
_PyOS_URandom(void *buffer, Py_ssize_t size)
{
    if (size < 0) {
        PyErr_Format(PyExc_ValueError,
                     "negative argument not allowed");
        return -1;
    }
    if (size == 0)
        return 0;

#ifdef MS_WINDOWS
    return win32_urandom((unsigned char *)buffer, size, 1);
#else
    return dev_urandom_python((char*)buffer, size);
#endif
}

void
_PyRandom_Init(void)
{
    char *env;
    unsigned char *secret = (unsigned char *)&_Py_HashSecret.uc;
    Py_ssize_t secret_size = sizeof(_Py_HashSecret_t);
    assert(secret_size == sizeof(_Py_HashSecret.uc));

    if (_Py_HashSecret_Initialized)
        return;
    _Py_HashSecret_Initialized = 1;

    /*
      Hash randomization is enabled.  Generate a per-process secret,
      using PYTHONHASHSEED if provided.
    */

    env = Py_GETENV("PYTHONHASHSEED");
    if (env && *env != '\0' && strcmp(env, "random") != 0) {
        char *endptr = env;
        unsigned long seed;
        seed = strtoul(env, &endptr, 10);
        if (*endptr != '\0'
            || seed > 4294967295UL
            || (errno == ERANGE && seed == ULONG_MAX))
        {
            Py_FatalError("PYTHONHASHSEED must be \"random\" or an integer "
                          "in range [0; 4294967295]");
        }
        if (seed == 0) {
            /* disable the randomized hash */
            memset(secret, 0, secret_size);
        }
        else {
            lcg_urandom(seed, secret, secret_size);
        }
    }
    else {
#ifdef MS_WINDOWS
        (void)win32_urandom(secret, secret_size, 0);
#else
        dev_urandom_noraise(secret, secret_size);
#endif
    }
}

void
_PyRandom_Fini(void)
{
#ifndef MS_WINDOWS
    dev_urandom_close();
#endif
}

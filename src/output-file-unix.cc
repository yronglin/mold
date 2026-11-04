#include "config.h"
#include "mold.h"

#include <fcntl.h>
#include <filesystem>
#include <sys/file.h>
#include <sys/mman.h>
#include <system_error>

#ifdef __linux__
# include <sys/vfs.h>
# include <linux/magic.h>
#endif

namespace mold {

// Returns the process's umask. umask(2) can only be read by setting it,
// so we restore the original value immediately.
static u32 get_umask() {
  u32 orig_umask = umask(0);
  umask(orig_umask);
  return orig_umask;
}

template <typename E>
static int
open_or_create_file(Context<E> &ctx, std::string path, std::string tmpfile,
                    int perm) {
  // Reuse an existing file if exists and writable because on Linux,
  // writing to an existing file is much faster than creating a fresh
  // file and writing to it.
  if (ctx.overwrite_output_file && rename(path.c_str(), tmpfile.c_str()) == 0) {
    i64 fd = ::open(tmpfile.c_str(), O_RDWR | O_CREAT, perm);
    if (fd != -1)
      return fd;
    unlink(tmpfile.c_str());
  }

  i64 fd = ::open(tmpfile.c_str(), O_RDWR | O_CREAT, perm);
  if (fd == -1)
    Fatal(ctx) << "cannot open " << tmpfile << ": " << errno_string();
  return fd;
}

// MemoryMappedOutputFile writes a regular file through a memory mapping of
// a temporary file that is renamed into place on close, so that a failed
// link doesn't leave a truncated output behind and a running executable
// isn't modified underneath the kernel.
template <typename E>
class MemoryMappedOutputFile : public OutputFile<E> {
public:
  MemoryMappedOutputFile(Context<E> &ctx, std::string path, i64 filesize, int perm)
    : OutputFile<E>(path, filesize, true) {
    std::string pid = std::to_string(getpid());
    std::string tmpfile = path_dirname(path) / ("."s + path_filename(path) + "." + pid);

    this->fd = open_or_create_file(ctx, path, tmpfile, perm);

    if (fchmod(this->fd, perm & ~get_umask()) == -1)
      Fatal(ctx) << "fchmod failed: " << errno_string();

    if (ftruncate(this->fd, filesize) == -1)
      Fatal(ctx) << "ftruncate failed: " << errno_string();

    // Register the temporary file so that cleanup() removes it on a fatal
    // error. The string must live until process exit, as a signal handler
    // may read it at any time.
    output_tmpfile = (char *)save_string(ctx, tmpfile).data();

#if HAVE_FALLOCATE
    // Calling fallocate speeds up later linking passes on ext4 by
    // taking disk block allocation out of the page-fault handler.
    // On tmpfs, it instead makes things much slower: the kernel
    // allocates and zeroes every page of the range inside the
    // syscall, on one thread, which takes ~1.8 s for a 5 GiB file.
    if (struct statfs fs;
        fstatfs(this->fd, &fs) || fs.f_type != TMPFS_MAGIC)
      fallocate(this->fd, 0, 0, filesize);
#endif

    // We map the file with twice as much address space as its size, so
    // that extend() can grow the file into the mapping in place.
    // Touching the mapping beyond the end of the file is not allowed,
    // but growing the file with ftruncate makes the tail of the
    // mapping accessible without any further mmap call.
    vasize = filesize * 2;
    this->buf = (u8 *)mmap(nullptr, vasize, PROT_READ | PROT_WRITE,
                           MAP_SHARED, this->fd, 0);

    if (this->buf == MAP_FAILED) {
      // If the address space is too tight, map just the file.
      vasize = filesize;
      this->buf = (u8 *)mmap(nullptr, filesize, PROT_READ | PROT_WRITE,
                             MAP_SHARED, this->fd, 0);
      if (this->buf == MAP_FAILED)
        Fatal(ctx) << path << ": mmap failed: " << errno_string();
    }

    mold::output_buffer_start = this->buf;
    mold::output_buffer_end = this->buf + filesize;
  }

  ~MemoryMappedOutputFile() {
    if (fd2 != -1)
      ::close(fd2);
  }

  // Extend the file so that the caller can fill the appended data
  // through the tail of the mapping. This is called at most once per
  // output file.
  u8 *extend(Context<E> &ctx, i64 size) override {
    i64 mapsize = this->filesize;

    if (ftruncate(this->fd, mapsize + size) == -1)
      Fatal(ctx) << "ftruncate failed: " << errno_string();

#if HAVE_FALLOCATE
    // Allocate only the appended range. Reallocating the already written
    // prefix can flush dirty extents on filesystems such as btrfs and
    // serialize a large part of .gdb_index output.
    if (struct statfs fs;
        fstatfs(this->fd, &fs) || fs.f_type != TMPFS_MAGIC)
      fallocate(this->fd, 0, mapsize, size);
#endif

    if (mapsize + size > vasize) {
      // The appended data does not fit in the mapping. Map the grown
      // file again, moving the buffer. Release the old mapping first, as
      // the address space may be too tight for both. The data written
      // through it stays in the file.
      munmap(this->buf, vasize);
      vasize = mapsize + size;

      this->buf = (u8 *)mmap(nullptr, vasize, PROT_READ | PROT_WRITE,
                             MAP_SHARED, this->fd, 0);
      if (this->buf == MAP_FAILED)
        Fatal(ctx) << this->path << ": mmap failed: " << errno_string();

      ctx.buf = this->buf;
      mold::output_buffer_start = this->buf;
    }

    this->filesize += size;
    mold::output_buffer_end = this->buf + mapsize + size;

#ifdef MADV_HUGEPAGE
    madvise(this->buf, mapsize + size, MADV_HUGEPAGE);
#endif
    return this->buf + mapsize;
  }

  void close(Context<E> &ctx) override {
    Timer t(ctx, "close_file");

    if (!this->is_unmapped)
      munmap(this->buf, vasize);
    ::close(this->fd);

    // If an output file already exists, open it and then remove it.
    // This is the fastest way to unlink a file, as it does not make the
    // system immediately release disk blocks occupied by the file.
    fd2 = ::open(this->path.c_str(), O_RDONLY);
    if (fd2 != -1)
      unlink(this->path.c_str());

    if (rename(output_tmpfile, this->path.c_str()) == -1)
      Fatal(ctx) << this->path << ": rename failed: " << errno_string();
    output_tmpfile = nullptr;
  }

private:
  int fd2 = -1;

  // Size of the file mapping, which may extend past the end of the file.
  i64 vasize = 0;
};

// Opens an output file of the given size.
//
// A regular file is written through a memory mapping of a temporary file
// that is renamed into place on close (see MemoryMappedOutputFile).
// Anything else, such as a device, a pipe or standard output, is
// assembled in memory and written out at the end.
template <typename E>
std::unique_ptr<OutputFile<E>>
OutputFile<E>::open(Context<E> &ctx, std::string path, i64 filesize, int perm) {
  Timer t(ctx, "open_file");

  if (path.starts_with('/') && !ctx.arg.chroot.empty())
    path = ctx.arg.chroot + "/" + path_clean(path);

  std::error_code error;
  bool is_special = path == "-" ||
                    (!std::filesystem::is_regular_file(path, error) && !error);

  OutputFile<E> *file;
  if (is_special)
    file = new MallocOutputFile(ctx, path, filesize, perm);
  else
    file = new MemoryMappedOutputFile(ctx, path, filesize, perm);

#ifdef MADV_HUGEPAGE
  // Enable transparent huge page for an output memory-mapped file.
  // Linking a Chromium debug build is ~20% faster with this madvise call.
  //
  // Without this, every 4 KiB page of the output takes its own page fault
  // when it is first written, and the faults of the many copying threads
  // serialize on the file's page cache. With it, the kernel backs the
  // mapping with large folios and the number of faults drops by an order
  // of magnitude.
  madvise(file->buf, filesize, MADV_HUGEPAGE);
#endif

  if (ctx.arg.filler != -1)
    memset(file->buf, ctx.arg.filler, filesize);
  return std::unique_ptr<OutputFile>(file);
}

// LockingOutputFile is similar to MemoryMappedOutputFile, but it doesn't
// rename output files and instead writes in place and acquires an
// exclusive file lock using flock(). This is used for a separate debug
// file that a debugger may wait on. The file is made unusable right away
// so that a stale one is not picked up by accident; resize() gives it its
// size.
template <typename E>
LockingOutputFile<E>::LockingOutputFile(Context<E> &ctx, std::string path,
                                        int perm)
  : OutputFile<E>(path, 0, true) {
  this->fd = ::open(path.c_str(), O_RDWR | O_CREAT, perm);
  if (this->fd == -1)
    Fatal(ctx) << "cannot open " << path << ": " << errno_string();
  flock(this->fd, LOCK_EX);

  // We may be overwriting an existing debug info file. We want to
  // make the file unusable so that gdb won't use it by accident until
  // it's ready.
  u8 buf[256] = {};
  (void)!!write(this->fd, buf, sizeof(buf));
}

// Sets the size of the file and maps it.
template <typename E>
void LockingOutputFile<E>::resize(Context<E> &ctx, i64 filesize) {
  if (ftruncate(this->fd, filesize) == -1)
    Fatal(ctx) << "ftruncate failed: " << errno_string();

  // As in MemoryMappedOutputFile, we map the file with twice as much
  // address space as its size so that extend() can grow the file into
  // the mapping in place.
  vasize = filesize * 2;
  this->buf = (u8 *)mmap(nullptr, vasize, PROT_READ | PROT_WRITE,
                         MAP_SHARED, this->fd, 0);

  if (this->buf == MAP_FAILED) {
    vasize = filesize;
    this->buf = (u8 *)mmap(nullptr, filesize, PROT_READ | PROT_WRITE,
                           MAP_SHARED, this->fd, 0);
    if (this->buf == MAP_FAILED)
      Fatal(ctx) << this->path << ": mmap failed: " << errno_string();
  }

  this->filesize = filesize;
  mold::output_buffer_start = this->buf;
  mold::output_buffer_end = this->buf + filesize;
}

template <typename E>
u8 *LockingOutputFile<E>::extend(Context<E> &ctx, i64 size) {
  i64 mapsize = this->filesize;

  if (ftruncate(this->fd, mapsize + size) == -1)
    Fatal(ctx) << "ftruncate failed: " << errno_string();

  if (mapsize + size > vasize) {
    // The appended data does not fit in the mapping. Map the grown
    // file again, moving the buffer.
    munmap(this->buf, vasize);
    vasize = mapsize + size;

    this->buf = (u8 *)mmap(nullptr, vasize, PROT_READ | PROT_WRITE,
                           MAP_SHARED, this->fd, 0);
    if (this->buf == MAP_FAILED)
      Fatal(ctx) << this->path << ": mmap failed: " << errno_string();

    ctx.buf = this->buf;
    mold::output_buffer_start = this->buf;
  }

  this->filesize += size;
  mold::output_buffer_end = this->buf + mapsize + size;
  return this->buf + mapsize;
}

template <typename E>
void LockingOutputFile<E>::close(Context<E> &ctx) {
  if (!this->is_unmapped)
    munmap(this->buf, vasize);
  ::close(this->fd);
}

using E = MOLD_TARGET;

template class OutputFile<E>;
template class LockingOutputFile<E>;

} // namespace mold

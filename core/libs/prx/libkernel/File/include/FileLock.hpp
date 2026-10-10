#ifndef CORE_LIBS_PRX_LIBKERNEL_FILE_FILELOCK_HPP
#define CORE_LIBS_PRX_LIBKERNEL_FILE_FILELOCK_HPP

#include "prx/libc/include/GuestFileDescriptors.hpp"

namespace File {

int Flock(const GuestFiles::Lease& owner, int operation);
void ForgetFileLock(int fd);

}

#endif

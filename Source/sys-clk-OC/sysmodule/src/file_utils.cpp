/*
 * --------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <p-sam@d3vs.net>, <natinusala@gmail.com>, <m4x@m4xw.net>
 * wrote this file. As long as you retain this notice you can do whatever you
 * want with this stuff. If you meet any of us some day, and you think this
 * stuff is worth it, you can buy us a beer in return.  - The sys-clk authors
 * --------------------------------------------------------------------------
 */

#include "file_utils.h"
#include "clocks.h"
#include <dirent.h>
#include <nxExt.h>

static LockableMutex g_log_mutex;
static LockableMutex g_csv_mutex;
static std::atomic_bool g_has_initialized = false;
static bool g_log_enabled = false;
static std::uint64_t g_last_flag_check = 0;

extern "C" void __libnx_init_time(void);

static void _FileUtils_InitializeThreadFunc(void *args)
{
    FileUtils::Initialize();
}

bool FileUtils::IsInitialized()
{
    return g_has_initialized;
}

void FileUtils::LogLine(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (g_has_initialized)
    {
        g_log_mutex.Lock();

        FileUtils::RefreshFlags(false);

        if(g_log_enabled)
        {
            FILE *file = fopen(FILE_LOG_FILE_PATH, "a");

            if (file)
            {
                struct timespec now;
                clock_gettime(CLOCK_REALTIME, &now);
                struct tm* nowTm = localtime(&now.tv_sec);

                fprintf(file, "[%04d-%02d-%02d %02d:%02d:%02d.%03ld] ", nowTm->tm_year+1900, nowTm->tm_mon+1, nowTm->tm_mday, nowTm->tm_hour, nowTm->tm_min, nowTm->tm_sec, now.tv_nsec / 1000000UL);
                vfprintf(file, format, args);
                fprintf(file, "\n");
                fclose(file);
            }
        }

        g_log_mutex.Unlock();
    }
    va_end(args);
}

void FileUtils::WriteContextToCsv(const SysClkContext* context)
{
    std::scoped_lock lock{g_csv_mutex};

    FILE *file = fopen(FILE_CONTEXT_CSV_PATH, "a");

    if (file)
    {
        // Print header
        if(!ftell(file))
        {
            fprintf(file, "timestamp,profile,app_tid");

            for (unsigned int module = 0; module < SysClkModule_EnumMax; module++)
            {
                fprintf(file, ",%s_hz", sysclkFormatModule((SysClkModule)module, false));
            }

            for (unsigned int sensor = 0; sensor < SysClkThermalSensor_EnumMax; sensor++)
            {
                fprintf(file, ",%s_milliC", sysclkFormatThermalSensor((SysClkThermalSensor)sensor, false));
            }

            fprintf(file, "\n");
        }

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);

        fprintf(file, "%ld%03ld,%s,%016lx", now.tv_sec, now.tv_nsec / 1000000UL, sysclkFormatProfile(context->profile, false), context->applicationId);

        for (unsigned int module = 0; module < SysClkModule_EnumMax; module++)
        {
            fprintf(file, ",%d", context->freqs[module]);
        }

        for (unsigned int sensor = 0; sensor < SysClkThermalSensor_EnumMax; sensor++)
        {
            fprintf(file, ",%d", context->temps[sensor]);
        }

        fprintf(file, "\n");
        fclose(file);
    }
}

void FileUtils::RefreshFlags(bool force)
{
    std::uint64_t now = armTicksToNs(armGetSystemTick());
    if(!force && (now - g_last_flag_check) < FILE_FLAG_CHECK_INTERVAL_NS)
    {
        return;
    }

    FILE *file = fopen(FILE_LOG_FLAG_PATH, "r");
    if (file)
    {
        g_log_enabled = true;
        fclose(file);
    } else {
        g_log_enabled = false;
    }

    g_last_flag_check = now;
}

void FileUtils::InitializeAsync()
{
    Thread initThread = {0};
    threadCreate(&initThread, _FileUtils_InitializeThreadFunc, NULL, NULL, 0x2000, 0x15, 0);
    threadStart(&initThread);
}

Result FileUtils::Initialize()
{
    Result rc = 0;

    if (R_SUCCEEDED(rc))
    {
        rc = timeInitialize();
    }

    __libnx_init_time();
    timeExit();

    if (R_SUCCEEDED(rc))
    {
        rc = fsInitialize();
    }

    if (R_SUCCEEDED(rc))
    {
        rc = fsdevMountSdmc();
    }

    if (R_SUCCEEDED(rc))
    {
        FileUtils::RefreshFlags(true);
        g_has_initialized = true;
        FileUtils::LogLine("=== " TARGET " " TARGET_VERSION " ===");
    }

    return rc;
}

void FileUtils::Exit()
{
    if (!g_has_initialized)
    {
        return;
    }

    g_has_initialized = false;
    g_log_enabled = false;

    fsdevUnmountAll();
    fsExit();
}

void FileUtils::ParseLoaderKip() {
    const char* dirs[] = { "/", "/atmosphere/", "/atmosphere/kips/", "/bootloader/" };
    char* full_path = new char[0x200];
    for (auto const& dir : dirs) {
        struct dirent *entry = NULL;
        DIR *dp = opendir(dir);
        if (!dp)
            continue;

        while ((entry = readdir(dp))) {
            if (entry->d_type != DT_REG)
                continue;

            snprintf(full_path, 0x200, "%s%s", dir, entry->d_name);

            FILE* fp = fopen(full_path, "r");
            if (!fp)
                continue;

            fseek(fp, 0, SEEK_END);
            long res = ftell(fp);
            fclose(fp);
            if (res == -1)
                continue;

            size_t filesize = (size_t)res;
            if (filesize < 0x1000 || filesize > 512 * 1024)
                continue;

            const char kip_ext[] = {'.', 'k', 'i', 'p'};
            size_t file_name_len = strnlen(reinterpret_cast<const char*>(&entry->d_name), 256);
            const char* file_ext = &entry->d_name[file_name_len - sizeof(kip_ext)];

            if (strncasecmp((const char*)kip_ext, file_ext, sizeof(kip_ext)))
                continue;

            if (R_SUCCEEDED(CustParser(full_path, filesize))) {
                LogLine("Parsed cust config from %s, maxMemFreq: %u MHz, boostCpuFreq: %u MHz",
                    full_path,
                    Clocks::maxMemFreq / 1'000'000,
                    Clocks::boostCpuFreq / 1'000'000
                );
                delete[] full_path;
                return;
            }
        }
        closedir(dp);
    }
    delete[] full_path;
}

Result FileUtils::CustParser(const char* filepath, size_t filesize) {
    enum ParseError {
        ParseError_Success = 0,
        ParseError_OpenReadFailed,
        ParseError_WrongKipMagic,
        ParseError_CustNotFound,
        ParseError_WrongCustRev,
    };

    FILE* fp = fopen(filepath, "r");
    if (!fp)
        return ParseError_OpenReadFailed;

    constexpr uint8_t KIP_MAGIC[] = {'K', 'I', 'P', '1', 'L', 'o', 'a', 'd', 'e', 'r'};
    constexpr size_t  BLOCK_SIZE = 0x1000;

    char* tmp_block = new char[BLOCK_SIZE];
    fread(tmp_block, sizeof(char), BLOCK_SIZE, fp);

    if (memcmp(KIP_MAGIC, tmp_block, sizeof(KIP_MAGIC))) {
        delete[] tmp_block;
        return ParseError_WrongKipMagic;
    }

    CustTable table {};

    fpos_t cust_pos = 0;
    long block_pos = 0;
    while ((block_pos = ftell(fp)) >= 0 && (size_t)block_pos < filesize) {
        for (size_t i = 0; i < BLOCK_SIZE; i += sizeof(table.cust)) {
            if (memcmp(table.cust, &tmp_block[i], sizeof(table.cust)))
                continue;

            fgetpos(fp, &cust_pos);
            cust_pos = cust_pos + i - BLOCK_SIZE;
            goto found;
        }
        fread(tmp_block, sizeof(char), BLOCK_SIZE, fp);
    }

  found:
    delete[] tmp_block;

    if (!cust_pos) {
        fclose(fp);
        return ParseError_CustNotFound;
    }

    memset(reinterpret_cast<void*>(&table), 0, sizeof(CustTable));
    fsetpos(fp, &cust_pos);
    if (!fread(reinterpret_cast<char*>(&table), 1, sizeof(CustTable), fp)) {
        fclose(fp);
        return ParseError_OpenReadFailed;
    }

    fclose(fp);

    if (table.custRev != 2)
        return ParseError_WrongCustRev;

    if (Clocks::GetIsMariko()) {
        if (table.marikoCpuBoostClock)
            Clocks::boostCpuFreq = table.marikoCpuBoostClock * 1000;
        if (table.marikoEmcMaxClock)
            Clocks::maxMemFreq = table.marikoEmcMaxClock * 1000;
    } else {
        if (table.eristaEmcMaxClock)
            Clocks::maxMemFreq = table.eristaEmcMaxClock * 1000;
    }

    return ParseError_Success;
}

Result FileUtils::mkdir_p(const char* dirpath) {
    // https://gist.github.com/JonathonReinhart/8c0d90191c38af2dcadb102c4e202950
    auto mkdir_wrapper = [](char* path) {
        errno = 0;
        size_t len = strnlen(path, 0x1000);
        bool isCWDir = (len == 0) || (len == 1 && (path[0] == '.' || path[0] == '/'));
        if (isCWDir)
            return 0;

        if (R_SUCCEEDED(mkdir(path, S_IRWXU)))
            return 0;

        struct stat st;
        if (errno == EEXIST &&
            R_SUCCEEDED(stat(path, &st)) &&
            S_ISDIR(st.st_mode)) {
            errno = 0;
            return 0;
        }

        errno = ENOTDIR;
        return -1;
    };

    errno = 0;
    Result res = 0;

    size_t path_len = strnlen(dirpath, 0x1000);
    char* path_copy = new char[path_len];
    memcpy(path_copy, dirpath, path_len);
    char* p = path_copy;
    while (*p) {
        if (*p == '/') {
            // Temporarily truncate
            *p = '\0';

            if (R_FAILED(mkdir_wrapper(path_copy))) {
                res = -1;
                goto end;
            }

            *p = '/';
        }
        p++;
    }

    if (R_FAILED(mkdir_wrapper(path_copy)))
        res = -1;

  end:
    delete[] path_copy;
    return res;
}

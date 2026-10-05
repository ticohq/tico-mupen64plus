/// @file TicoSafeFile.h
/// @brief Writing saves and states without ever losing the previous one.
///
/// A file is written beside its destination first (name.tmp), flushed to the
/// card, and only then takes the destination's place, so a crash or a full
/// card mid-write leaves the old file intact. The file it replaces moves into
/// a backups/ folder beside it as name.1, the one before that becomes name.2,
/// and so on up to the number kept. Writing what the file already holds does
/// nothing, so identical saves never push good backups out.
#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace TicoSafeFile
{

inline bool Exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

/// dir/backups/<name>.<n>
inline std::string BackupPath(const std::string &path, int n)
{
    const size_t slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    return dir + "backups/" + name + "." + std::to_string(n);
}

inline void MakeBackupDir(const std::string &path)
{
    const std::string backup = BackupPath(path, 1);
    mkdir(backup.substr(0, backup.find_last_of('/')).c_str(), 0777);
}

inline bool ReadAll(const std::string &path, std::vector<unsigned char> &out)
{
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
        return false;
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    out.resize(size > 0 ? (size_t)size : 0);
    const bool ok = size >= 0 && fread(out.data(), 1, out.size(), fp) == out.size();
    fclose(fp);
    return ok;
}

inline bool SameContents(const std::string &path, const void *data, size_t size)
{
    std::vector<unsigned char> current;
    return ReadAll(path, current) && current.size() == size &&
           std::equal(current.begin(), current.end(), (const unsigned char *)data);
}

/// Moves @p path to backups/<name>.1, shifting older backups up and dropping
/// the one past @p keep. With keep 0 the file is just removed.
inline void Rotate(const std::string &path, int keep)
{
    if (keep <= 0)
    {
        remove(path.c_str());
        return;
    }
    MakeBackupDir(path);
    remove(BackupPath(path, keep).c_str());
    for (int n = keep - 1; n >= 1; --n)
        rename(BackupPath(path, n).c_str(), BackupPath(path, n + 1).c_str());
    rename(path.c_str(), BackupPath(path, 1).c_str());
}

/// Writes @p data to @p path safely, keeping @p keep earlier versions.
inline bool Write(const std::string &path, const void *data, size_t size, int keep)
{
    if (Exists(path) && SameContents(path, data, size))
        return true;
    const std::string tmp = path + ".tmp";
    FILE *fp = fopen(tmp.c_str(), "wb");
    if (!fp)
        return false;
    bool ok = fwrite(data, 1, size, fp) == size;
    ok = fflush(fp) == 0 && ok;
    fsync(fileno(fp)); // best effort: not every filesystem driver has it
    ok = fclose(fp) == 0 && ok;
    if (!ok)
    {
        remove(tmp.c_str()); // the old file is untouched
        return false;
    }
    if (Exists(path))
        Rotate(path, keep);
    return rename(tmp.c_str(), path.c_str()) == 0;
}

/// Copies @p path into its backups (for a file the core keeps open and writes
/// in place), unless the newest backup already matches it.
inline void BackupCopy(const std::string &path, int keep)
{
    std::vector<unsigned char> data;
    if (keep <= 0 || !ReadAll(path, data) || data.empty())
        return;
    if (SameContents(BackupPath(path, 1), data.data(), data.size()))
        return;
    MakeBackupDir(path);
    remove(BackupPath(path, keep).c_str());
    for (int n = keep - 1; n >= 1; --n)
        rename(BackupPath(path, n).c_str(), BackupPath(path, n + 1).c_str());
    if (FILE *fp = fopen(BackupPath(path, 1).c_str(), "wb"))
    {
        fwrite(data.data(), 1, data.size(), fp);
        fclose(fp);
    }
}

} // namespace TicoSafeFile

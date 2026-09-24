#include "core/Migrate.h"

#include "core/Assets.h"
#include "core/DcOverlay.h"
#include "core/DiscImage.h"
#include "core/Util.h"
#include "core/Video.h"

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace re1 {
namespace {

bool ensureTarget(const std::string& targetRoot, std::string* error) {
    if (targetRoot.empty()) {
        if (error) *error = "no target game folder selected";
        return false;
    }
    if (!makeDirs(targetRoot)) {
        if (error) *error = "cannot create the target folder " + targetRoot;
        return false;
    }
    return true;
}

}  // namespace

bool migratePcAssets(const PcMigrationOptions& opts, const Progress& progress,
                     std::string* error) {
    progress.info("PC asset migration");
    if (opts.sourcePath.empty()) {
        if (error) *error = "no source selected";
        return false;
    }
    if (!ensureTarget(opts.targetRoot, error)) return false;

    const std::string destRoot =
        joinPath(opts.targetRoot, versionName(opts.version));
    progress.info("source: " + opts.sourcePath +
                  (opts.sourceIsImage ? " (disc image)" : " (folder)"));
    progress.info("target: " + destRoot);
    if (!makeDirs(destRoot)) {
        if (error) *error = "cannot create " + destRoot;
        return false;
    }

    if (opts.sourceIsImage) {
        DiscImage img;
        std::string err;
        if (!img.open(opts.sourcePath, &err)) {
            if (error) *error = err;
            return false;
        }
        progress.info(img.isRaw() ? "image: raw 2352-byte sectors"
                                  : "image: 2048-byte ISO");
        if (!extractAssetsFromImage(img, destRoot, progress, error)) return false;
    } else {
        std::string root;
        if (!findAssetRootInDir(opts.sourcePath, &root, error)) return false;
        progress.info("asset root: " + root);
        if (!copyAssetsFromDir(root, destRoot, progress, error)) return false;
    }

    if (opts.convertMovies) {
        progress.info("movies: AVI -> MP4");
        if (!convertPcMovies(joinPath(destRoot, "Movie"), opts.ffmpegPath,
                             opts.keepAvi, progress, error))
            return false;
    }

    progress.info("done: " + destRoot);
    return true;
}

bool migrateDcAssets(const DcMigrationOptions& opts, const Progress& progress,
                     std::string* error) {
    progress.info("Director's Cut migration");
    if (opts.imagePath.empty()) {
        if (error) *error = "no disc image selected";
        return false;
    }
    if (!ensureTarget(opts.targetRoot, error)) return false;

    DiscImage img;
    std::string err;
    if (!img.open(opts.imagePath, &err)) {
        if (error) *error = err;
        return false;
    }
    if (!img.isRaw())
        progress.info("warning: 2048-byte image; use a raw .bin/.cue so the "
                      "movie audio is intact");
    else
        progress.info("image: raw 2352-byte sectors (CD-XA audio intact)");

    const std::string overlay = joinPath(opts.targetRoot, "DC");
    const std::string baseTree =
        joinPath(opts.targetRoot, versionName(opts.base));

    std::error_code ec;
    const std::string tmp =
        joinPath(fs::temp_directory_path(ec).string(), "re1am_psxdc");
    fs::remove_all(tmp, ec);
    if (!makeDirs(tmp)) {
        if (error) *error = "cannot create " + tmp;
        return false;
    }

    auto cleanup = [&]() { fs::remove_all(tmp, ec); };

    std::vector<std::string> folders = {"DATA",   "ENEMY",   "ITEM_M1",
                                        "ITEM_M2", "PLAYERS"};
    for (char c : std::string("1234567")) folders.push_back(std::string("STAGE") + c);
    for (char c : std::string("89ABCDE")) folders.push_back(std::string("STAGE") + c);

    progress.info("extracting the disc's asset folders...");
    if (!extractFoldersFromImage(img, folders, tmp, progress, error)) {
        cleanup();
        return false;
    }

    DcOverlayOptions o;
    o.sourceDir = tmp;
    o.overlayDir = overlay;
    o.baseTreeDir = baseTree;
    o.baseName = versionName(opts.base);
    o.backgrounds = opts.backgrounds;

    if (!buildDcOverlay(o, progress, error)) {
        cleanup();
        return false;
    }

    if (opts.convertMovies) {
        progress.info("movies: STR -> MP4");
        const std::string movieOut = joinPath(overlay, "Movie");
        makeDirs(movieOut);
        const std::vector<std::string> pcDirs = {
            joinPath(opts.targetRoot, "USA/Movie"),
            joinPath(opts.targetRoot, "JPN/Movie")};
        size_t converted = 0;
        for (const auto& e : img.entries()) {
            if (e.directory) continue;
            if (extensionOf(e.path) != ".str") continue;
            if (toUpper(e.path).find("MOVIE") == std::string::npos) continue;
            if (e.size == 0 || e.size % 2048 != 0) continue;
            if (progress.isCancelled()) {
                if (error) *error = "cancelled";
                cleanup();
                return false;
            }
            if (!convertStrMovie(img, e, movieOut, opts.ffmpegPath,
                                 progress, error)) {
                cleanup();
                return false;
            }
            ++converted;
        }
        progress.info("  " + std::to_string(converted) + " movie(s) converted");
    }

    if (opts.verify) {
        if (!verifyDcOverlay(o, progress, error)) {
            cleanup();
            return false;
        }
    }

    cleanup();
    progress.info("done: " + overlay);
    return true;
}

}  // namespace re1

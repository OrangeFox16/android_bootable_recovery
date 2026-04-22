#include "twrpApex.hpp"
#include "twrp-functions.hpp"
#include "common.h"

#include <filesystem>
#include <regex>
#include <android-base/properties.h>
#include <ziparchive/zip_archive.h>

namespace fs = std::filesystem;

bool twrpApex::loadApexImages() {
	std::vector<std::string> apexFiles;
	std::vector<std::string> checkApexFlatFiles;

#ifdef TW_ADDITIONAL_APEX_FILES
	char* additionalFiles = strdup(EXPAND(TW_ADDITIONAL_APEX_FILES));
	char* additionalApexFiles = std::strtok(additionalFiles, " ");
#endif

	apexFiles.push_back(APEX_DIR "/com.android.apex.cts.shim.apex");
	apexFiles.push_back(APEX_DIR "/com.google.android.tzdata2.apex");
	apexFiles.push_back(APEX_DIR "/com.android.tzdata.apex");
	apexFiles.push_back(APEX_DIR "/com.android.art.release.apex");
	apexFiles.push_back(APEX_DIR "/com.google.android.media.swcodec.apex");
	apexFiles.push_back(APEX_DIR "/com.android.media.swcodec.apex");

#ifdef TW_ADDITIONAL_APEX_FILES
	while (additionalApexFiles) {
		std::stringstream apexFile;
		apexFile << APEX_DIR << "/" << additionalApexFiles;
		apexFiles.push_back(apexFile.str());
		additionalApexFiles = std::strtok(nullptr, " ");
	}
	free(additionalFiles);
#endif

	if (access(APEX_DIR, F_OK) != 0) {
		LOGERR("Unable to open %s\n", APEX_DIR);
		return false;
	}

	for (const auto& entry : fs::directory_iterator(APEX_DIR)) {
		if (entry.is_regular_file()) {
			checkApexFlatFiles.push_back(entry.path().string());
		}
	}

	if (checkApexFlatFiles.empty()) {
		LOGINFO("Bind mounting flattened apex directory\n");
		if (mount(APEX_DIR, APEX_BASE, "", MS_BIND, NULL) < 0) {
			LOGERR("Unable to bind mount flattened apex directory\n");
			return false;
		}
		android::base::SetProperty("twrp.apex.flattened", "true");
		return true;
	}

	if (!mountApexOnLoopbackDevices(apexFiles)) {
		LOGERR("Unable to create loop devices to mount apex files\n");
		return false;
	}

	return true;
}

std::string twrpApex::unzipImage(std::string file) {
	ZipArchiveHandle handle;
	if (OpenArchive(file.c_str(), &handle) != 0) {
		LOGINFO("unable to open zip archive %s\n", file.c_str());
		return {};
	}

	ZipEntry entry;
	if (FindEntry(handle, APEX_PAYLOAD, &entry) != 0) {
		LOGERR("unable to find %s in zip\n", APEX_PAYLOAD);
		CloseArchive(handle);
		return {};
	}

	std::string baseFile = basename(file.c_str());
	std::string path = "/tmp/" + baseFile;

	int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		LOGERR("unable to create %s\n", path.c_str());
		CloseArchive(handle);
		return {};
	}

	if (ExtractEntryToFile(handle, &entry, fd) != 0) {
		LOGERR("unable to extract %s\n", path.c_str());
		close(fd);
		CloseArchive(handle);
		return {};
	}

	close(fd);
	CloseArchive(handle);
	return path;
}

bool twrpApex::mountApexOnLoopbackDevices(std::vector<std::string> apexFiles) {
	int ctl_fd = open(LOOP_CONTROL, O_RDWR | O_CLOEXEC);
	if (ctl_fd < 0) {
		LOGERR("Unable to open %s: %s\n", LOOP_CONTROL, strerror(errno));
		return false;
	}

	for (auto&& apexFile : apexFiles) {
		int loop_num = ioctl(ctl_fd, LOOP_CTL_GET_FREE);
		if (loop_num < 0) {
			LOGERR("LOOP_CTL_GET_FREE failed: %s\n", strerror(errno));
			close(ctl_fd);
			return false;
		}

		std::string fileToMount = unzipImage(apexFile);
		if (fileToMount.empty()) {
			LOGINFO("Skipping apex: %s\n", apexFile.c_str());
			continue;
		}

		if (!loadApexImage(fileToMount, loop_num)) {
			LOGERR("Failed to load apex: %s\n", apexFile.c_str());
			close(ctl_fd);
			return false;
		}
	}

	close(ctl_fd);
	return true;
}

bool twrpApex::loadApexImage(std::string fileToMount, int loop_num) {
	struct loop_info64 info{};
	int fd = -1;
	int loop_fd = -1;

	fd = open(fileToMount.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		LOGERR("unable to open apex image: %s. Reason: %s\n",
			fileToMount.c_str(), strerror(errno));
		return false;
	}

	std::string loop_device = std::string(LOOP_BLOCK_DEVICE_DIR) + "loop" + std::to_string(loop_num);

	if (!TWFunc::Path_Exists(loop_device)) {
		if (mknod(loop_device.c_str(), S_IFBLK | 0600, makedev(7, loop_num)) != 0) {
			LOGERR("Failed to create loop device: %s\n", loop_device.c_str());
			close(fd);
			return false;
		}
	}

	loop_fd = open(loop_device.c_str(), O_RDWR);
	if (loop_fd < 0) {
		LOGERR("unable to open loop device: %s. Reason: %s\n",
			loop_device.c_str(), strerror(errno));
		close(fd);
		return false;
	}

	if (ioctl(loop_fd, LOOP_SET_FD, fd) < 0) {
		LOGERR("LOOP_SET_FD failed for %s: %s\n",
			fileToMount.c_str(), strerror(errno));
		close(fd);
		close(loop_fd);
		return false;
	}

	memset(&info, 0, sizeof(info));
	strlcpy((char*)info.lo_crypt_name, "twrpApex", LO_NAME_SIZE);

	if (ioctl(loop_fd, LOOP_SET_STATUS64, &info) < 0) {
		LOGERR("LOOP_SET_STATUS64 failed: %s\n", strerror(errno));
		ioctl(loop_fd, LOOP_CLR_FD, 0);
		close(fd);
		close(loop_fd);
		return false;
	}

	close(fd);

	std::string apex_name = basename(fileToMount.c_str());
	apex_name = std::regex_replace(apex_name, std::regex("\\.apex$"), "");

	std::string mount_point = std::string(APEX_BASE) + apex_name;

	if (mkdir(mount_point.c_str(), 0755) != 0 && errno != EEXIST) {
		LOGERR("mkdir failed: %s\n", mount_point.c_str());
		ioctl(loop_fd, LOOP_CLR_FD, 0);
		close(loop_fd);
		return false;
	}

	if (mount(loop_device.c_str(), mount_point.c_str(), "ext4", MS_RDONLY, nullptr) == 0) {
		LOGINFO("Mounted %s as ext4\n", apex_name.c_str());
		close(loop_fd);
		return true;
	}

	if (mount(loop_device.c_str(), mount_point.c_str(), "squashfs", MS_RDONLY, nullptr) == 0) {
		LOGINFO("Mounted %s as squashfs\n", apex_name.c_str());
		close(loop_fd);
		return true;
	}

	LOGERR("Failed to mount %s: %s\n", fileToMount.c_str(), strerror(errno));

	ioctl(loop_fd, LOOP_CLR_FD, 0);
	close(loop_fd);
	return false;
}

bool twrpApex::Unmount() {
	return (PartitionManager.UnMount_By_Path(APEX_BASE, false, MNT_DETACH) == 0);
}

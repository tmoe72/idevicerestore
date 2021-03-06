/*
 * idevicerestore.c
 * Restore device firmware and filesystem
 *
 * Copyright (c) 2010 Joshua Hill. All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <plist/plist.h>
#include <libirecovery.h>
#include <libimobiledevice/restore.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/libimobiledevice.h>

#include "dfu.h"
#include "tss.h"
#include "img3.h"
#include "ipsw.h"
#include "normal.h"
#include "restore.h"
#include "recovery.h"
#include "idevicerestore.h"

#define UNKNOWN_MODE   0
#define DFU_MODE       1
#define NORMAL_MODE    2
#define RECOVERY_MODE  3
#define RESTORE_MODE   4

int idevicerestore_debug = 0;
static int idevicerestore_mode = 0;
static int idevicerestore_quit = 0;
static int idevicerestore_custom = 0;

void usage(int argc, char* argv[]);
int write_file(const char* filename, char* data, int size);
int get_tss_data_by_name(plist_t tss, const char* entry, char** path, char** blob);
int get_tss_data_by_path(plist_t tss, const char* path, char** name, char** blob);
void device_callback(const idevice_event_t* event, void *user_data);
int get_signed_component_by_name(char* ipsw, plist_t tss, char* component, char** pdata, int* psize);
int get_signed_component_by_path(char* ipsw, plist_t tss, char* path, char** pdata, int* psize);

int main(int argc, char* argv[]) {
	int opt = 0;
	char* ipsw = NULL;
	char* uuid = NULL;
	uint64_t ecid = 0;
	while ((opt = getopt(argc, argv, "vdhcu:")) > 0) {
		switch (opt) {
		case 'h':
			usage(argc, argv);
			break;

		case 'v':
			idevicerestore_debug += 1;
			break;

		case 'c':
			idevicerestore_custom = 1;
			break;

		case 'd':
			idevicerestore_debug = 3;
			break;

		case 'u':
			uuid = optarg;
			break;

		default:
			usage(argc, argv);
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc == 1)
		ipsw = argv[0];

	if (ipsw == NULL) {
		error("ERROR: Please supply an IPSW\n");
		return -1;
	}

	idevice_t device = NULL;
	irecv_client_t recovery = NULL;
	lockdownd_client_t lockdown = NULL;
	irecv_error_t recovery_error = IRECV_E_SUCCESS;
	idevice_error_t device_error = IDEVICE_E_SUCCESS;
	lockdownd_error_t lockdown_error = LOCKDOWN_E_SUCCESS;

	/* determine recovery or normal mode */
	info("Checking for device in normal mode...\n");
	device_error = idevice_new(&device, uuid);
	if (device_error != IDEVICE_E_SUCCESS) {
		info("Checking for the device in recovery mode...\n");
		recovery_error = irecv_open(&recovery);
		if (recovery_error != IRECV_E_SUCCESS) {
			error("ERROR: Unable to find device, is it plugged in?\n");
			return -1;
		}
		info("Found device in recovery mode\n");
		idevicerestore_mode = RECOVERY_MODE;

	} else {
		info("Found device in normal mode\n");
		idevicerestore_mode = NORMAL_MODE;
	}

	/* retrieve ECID */
	if (idevicerestore_mode == NORMAL_MODE) {
		lockdown_error = lockdownd_client_new_with_handshake(device, &lockdown, "idevicerestore");
		if (lockdown_error != LOCKDOWN_E_SUCCESS) {
			error("ERROR: Unable to connect to lockdownd\n");
			idevice_free(device);
			return -1;
		}

		plist_t unique_chip_node = NULL;
		lockdown_error = lockdownd_get_value(lockdown, NULL, "UniqueChipID", &unique_chip_node);
		if (lockdown_error != LOCKDOWN_E_SUCCESS) {
			error("ERROR: Unable to get UniqueChipID from lockdownd\n");
			lockdownd_client_free(lockdown);
			idevice_free(device);
			return -1;
		}

		if (!unique_chip_node || plist_get_node_type(unique_chip_node) != PLIST_UINT) {
			error("ERROR: Unable to get ECID\n");
			lockdownd_client_free(lockdown);
			idevice_free(device);
			return -1;
		}

		plist_get_uint_val(unique_chip_node, &ecid);
		lockdownd_client_free(lockdown);
		plist_free(unique_chip_node);
		idevice_free(device);
		lockdown = NULL;
		device = NULL;

	} else if (idevicerestore_mode == RECOVERY_MODE) {
		recovery_error = irecv_get_ecid(recovery, &ecid);
		if (recovery_error != IRECV_E_SUCCESS) {
			error("ERROR: Unable to get device ECID\n");
			irecv_close(recovery);
			return -1;
		}
		irecv_close(recovery);
		recovery = NULL;
	}

	if (ecid != 0) {
		info("Found ECID %llu\n", ecid);
	} else {
		error("Unable to find device ECID\n");
		return -1;
	}

	/* parse buildmanifest */
	int buildmanifest_size = 0;
	char* buildmanifest_data = NULL;
	info("Extracting BuildManifest.plist from IPSW\n");
	if (ipsw_extract_to_memory(ipsw, "BuildManifest.plist", &buildmanifest_data, &buildmanifest_size) < 0) {
		error("ERROR: Unable to extract BuildManifest.plist IPSW\n");
		return -1;
	}

	plist_t manifest = NULL;
	plist_from_xml(buildmanifest_data, buildmanifest_size, &manifest);

	info("Creating TSS request\n");
	plist_t tss_request = tss_create_request(manifest, ecid);
	if (tss_request == NULL) {
		error("ERROR: Unable to create TSS request\n");
		plist_free(manifest);
		return -1;
	}
	plist_free(manifest);

	info("Sending TSS request\n");
	plist_t tss_response = tss_send_request(tss_request);
	if (tss_response == NULL) {
		error("ERROR: Unable to get response from TSS server\n");
		plist_free(tss_request);
		return -1;
	}
	info("Got TSS response\n");

	// Get name of filesystem DMG in IPSW
	char* filesystem = NULL;
	plist_t filesystem_node = plist_dict_get_item(tss_request, "OS");
	if (!filesystem_node || plist_get_node_type(filesystem_node) != PLIST_DICT) {
		error("ERROR: Unable to find filesystem node\n");
		plist_free(tss_request);
		return -1;
	}

	plist_t filesystem_info_node = plist_dict_get_item(filesystem_node, "Info");
	if (!filesystem_info_node || plist_get_node_type(filesystem_info_node) != PLIST_DICT) {
		error("ERROR: Unable to find filesystem info node\n");
		plist_free(tss_request);
		return -1;
	}

	plist_t filesystem_info_path_node = plist_dict_get_item(filesystem_info_node, "Path");
	if (!filesystem_info_path_node || plist_get_node_type(filesystem_info_path_node) != PLIST_STRING) {
		error("ERROR: Unable to find filesystem info path node\n");
		plist_free(tss_request);
		return -1;
	}
	plist_get_string_val(filesystem_info_path_node, &filesystem);
	plist_free(tss_request);

	info("Extracting filesystem from IPSW\n");
	if (ipsw_extract_to_file(ipsw, filesystem, filesystem) < 0) {
		error("ERROR: Unable to extract filesystem\n");
		return -1;
	}

	/* place device into recovery mode if required */
	if (idevicerestore_mode == NORMAL_MODE) {
		info("Entering recovery mode...\n");
		device_error = idevice_new(&device, uuid);
		if (device_error != IDEVICE_E_SUCCESS) {
			error("ERROR: Unable to find device\n");
			plist_free(tss_response);
			return -1;
		}

		lockdown_error = lockdownd_client_new_with_handshake(device, &lockdown, "idevicerestore");
		if (lockdown_error != LOCKDOWN_E_SUCCESS) {
			error("ERROR: Unable to connect to lockdownd service\n");
			plist_free(tss_response);
			idevice_free(device);
			return -1;
		}

		lockdown_error = lockdownd_enter_recovery(lockdown);
		if (lockdown_error != LOCKDOWN_E_SUCCESS) {
			error("ERROR: Unable to place device in recovery mode\n");
			lockdownd_client_free(lockdown);
			plist_free(tss_response);
			idevice_free(device);
			return -1;
		}

		lockdownd_client_free(lockdown);
		idevice_free(device);
		lockdown = NULL;
		device = NULL;
	}

	/* upload data to make device boot restore mode */
	if (recovery_send_ibec(ipsw, tss_response) < 0) {
		error("ERROR: Unable to send iBEC\n");
		plist_free(tss_response);
		return -1;
	}
	sleep(1);

	if (recovery_send_applelogo(ipsw, tss_response) < 0) {
		error("ERROR: Unable to send AppleLogo\n");
		plist_free(tss_response);
		return -1;
	}

	if (recovery_send_devicetree(ipsw, tss_response) < 0) {
		error("ERROR: Unable to send DeviceTree\n");
		plist_free(tss_response);
		return -1;
	}

	if (recovery_send_ramdisk(ipsw, tss_response) < 0) {
		error("ERROR: Unable to send Ramdisk\n");
		plist_free(tss_response);
		return -1;
	}

	// for some reason iboot requires a hard reset after ramdisk
	//   or things start getting wacky
	printf("Please unplug your device, then plug it back in\n");
	printf("Hit any key to continue...");
	getchar();

	if (recovery_send_kernelcache(ipsw, tss_response) < 0) {
		error("ERROR: Unable to send KernelCache\n");
		plist_free(tss_response);
		return -1;
	}

	idevice_event_subscribe(&device_callback, NULL);
	info("Waiting for device to enter restore mode\n");
	// block program until device has entered restore mode
	while (idevicerestore_mode != RESTORE_MODE) {
		sleep(1);
	}

	device_error = idevice_new(&device, uuid);
	if (device_error != IDEVICE_E_SUCCESS) {
		error("ERROR: Unable to open device\n");
		plist_free(tss_response);
		return -1;
	}

	restored_client_t restore = NULL;
	restored_error_t restore_error = restored_client_new(device, &restore, "idevicerestore");
	if (restore_error != RESTORE_E_SUCCESS) {
		error("ERROR: Unable to start restored client\n");
		plist_free(tss_response);
		idevice_free(device);
		return -1;
	}

	char* type = NULL;
	uint64_t version = 0;
	if (restored_query_type(restore, &type, &version) != RESTORE_E_SUCCESS) {
		error("ERROR: Device is not in restore mode. QueryType returned \"%s\"\n", type);
		plist_free(tss_response);
		restored_client_free(restore);
		idevice_free(device);
		return -1;
	}
	info("Device has successfully entered restore mode\n");

	/* start restore process */
	char* kernelcache = NULL;
	info("Restore protocol version is %llu.\n", version);
	restore_error = restored_start_restore(restore);
	if (restore_error == RESTORE_E_SUCCESS) {
		while (!idevicerestore_quit) {
			plist_t message = NULL;
			restore_error = restored_receive(restore, &message);
			plist_t msgtype_node = plist_dict_get_item(message, "MsgType");
			if (msgtype_node && PLIST_STRING == plist_get_node_type(msgtype_node)) {
				char *msgtype = NULL;
				plist_get_string_val(msgtype_node, &msgtype);
				if (!strcmp(msgtype, "ProgressMsg")) {
					restore_error = restore_handle_progress_msg(restore, message);

				} else if (!strcmp(msgtype, "DataRequestMsg")) {
					// device is requesting data to be sent
					plist_t datatype_node = plist_dict_get_item(message, "DataType");
					if (datatype_node && PLIST_STRING == plist_get_node_type(datatype_node)) {
						char *datatype = NULL;
						plist_get_string_val(datatype_node, &datatype);
						if (!strcmp(datatype, "SystemImageData")) {
							asr_send_system_image_data_from_file(device, restore, filesystem);

						} else if (!strcmp(datatype, "KernelCache")) {
							int kernelcache_size = 0;
							char* kernelcache_data = NULL;
							if (get_signed_component_by_name(ipsw, tss_response, "KernelCache", &kernelcache_data, &kernelcache_size) < 0) {
								error("ERROR: Unable to get kernelcache file\n");
								return -1;
							}
							restore_send_kernelcache(restore, kernelcache_data, kernelcache_size);
							free(kernelcache_data);

						} else if (!strcmp(datatype, "NORData")) {
							restore_send_nor_data(restore, ipsw, tss_response);

						} else {
							// Unknown DataType!!
							error("Unknown DataType\n");
							return -1;
						}
					}

				} else if (!strcmp(msgtype, "StatusMsg")) {
					restore_error = restore_handle_status_msg(restore, message);

				} else {
					info("Received unknown message type: %s\n", msgtype);
				}
			}

			if (RESTORE_E_SUCCESS != restore_error) {
				error("Invalid return status %d\n", restore_error);
				//idevicerestore_quit = 1;
			}

			plist_free(message);
		}
	} else {
		error("ERROR: Could not start restore. %d\n", restore_error);
	}

	restored_client_free(restore);
	plist_free(tss_response);
	idevice_free(device);
	unlink(filesystem);
	return 0;
}

void device_callback(const idevice_event_t* event, void *user_data) {
	if (event->event == IDEVICE_DEVICE_ADD) {
		idevicerestore_mode = RESTORE_MODE;
	} else if(event->event == IDEVICE_DEVICE_REMOVE) {
		idevicerestore_quit = 1;
	}
}

void usage(int argc, char* argv[]) {
	char *name = strrchr(argv[0], '/');
	printf("Usage: %s [OPTIONS] FILE\n", (name ? name + 1 : argv[0]));
	printf("Restore/upgrade IPSW firmware FILE to an iPhone/iPod Touch.\n");
	printf("  -d, \t\tenable communication debugging\n");
	printf("  -u, \t\ttarget specific device by its 40-digit device UUID\n");
	printf("  -h, \t\tprints usage information\n");
	printf("  -c, \t\trestore with a custom firmware\n");
	printf("  -v, \t\tenable incremental levels of verboseness\n");
	printf("\n");
	exit(1);
}

int write_file(const char* filename, char* data, int size) {
	debug("Writing data to %s\n", filename);
	FILE* file = fopen(filename, "wb");
	if (file == NULL) {
		error("read_file: Unable to open file %s\n", filename);
		return -1;
	}

	int bytes = fwrite(data, 1, size, file);
	fclose(file);

	if (bytes != size) {
		error("ERROR: Unable to write entire file: %s: %d %d\n", filename, bytes, size);
		return -1;
	}

	return size;
}

int get_tss_data_by_path(plist_t tss, const char* path, char** pname, char** pblob) {
	*pname = NULL;
	*pblob = NULL;

	char* key = NULL;
	plist_t tss_entry = NULL;
	plist_dict_iter iter = NULL;
	plist_dict_new_iter(tss, &iter);
	while (1) {
		plist_dict_next_item(tss, iter, &key, &tss_entry);
		if (key == NULL)
			break;

		if (!tss_entry || plist_get_node_type(tss_entry) != PLIST_DICT) {
			continue;
		}

		char* entry_path = NULL;
		plist_t entry_path_node = plist_dict_get_item(tss_entry, "Path");
		if (!entry_path_node || plist_get_node_type(entry_path_node) != PLIST_STRING) {
			error("ERROR: Unable to find TSS path node in entry %s\n", key);
			return -1;
		}
		plist_get_string_val(entry_path_node, &entry_path);
		if (strcmp(path, entry_path) == 0) {
			char* blob = NULL;
			uint64_t blob_size = 0;
			plist_t blob_node = plist_dict_get_item(tss_entry, "Blob");
			if (!blob_node || plist_get_node_type(blob_node) != PLIST_DATA) {
				error("ERROR: Unable to find TSS blob node in entry %s\n", key);
				return -1;
			}
			plist_get_data_val(blob_node, &blob, &blob_size);
			*pname = key;
			*pblob = blob;
			return 0;
		}

		free(key);
	}
	plist_free(tss_entry);

	return -1;
}

int get_tss_data_by_name(plist_t tss, const char* entry, char** ppath, char** pblob) {
	*ppath = NULL;
	*pblob = NULL;

	plist_t node = plist_dict_get_item(tss, entry);
	if (!node || plist_get_node_type(node) != PLIST_DICT) {
		error("ERROR: Unable to find %s entry in TSS response\n", entry);
		return -1;
	}

	char* path = NULL;
	plist_t path_node = plist_dict_get_item(node, "Path");
	if (!path_node || plist_get_node_type(path_node) != PLIST_STRING) {
		error("ERROR: Unable to find %s path in entry\n", path);
		return -1;
	}
	plist_get_string_val(path_node, &path);

	char* blob = NULL;
	uint64_t blob_size = 0;
	plist_t blob_node = plist_dict_get_item(node, "Blob");
	if (!blob_node || plist_get_node_type(blob_node) != PLIST_DATA) {
		error("ERROR: Unable to find %s blob in entry\n", path);
		free(path);
		return -1;
	}
	plist_get_data_val(blob_node, &blob, &blob_size);

	*ppath = path;
	*pblob = blob;
	return 0;
}

int get_signed_component_by_name(char* ipsw, plist_t tss, char* component, char** pdata, int* psize) {
	int size = 0;
	char* data = NULL;
	char* path = NULL;
	char* blob = NULL;
	img3_file* img3 = NULL;
	irecv_error_t error = 0;

	info("Extracting %s from TSS response\n", component);
	if (get_tss_data_by_name(tss, component, &path, &blob) < 0) {
		error("ERROR: Unable to get data for TSS %s entry\n", component);
		return -1;
	}

	info("Extracting %s from %s\n", path, ipsw);
	if (ipsw_extract_to_memory(ipsw, path, &data, &size) < 0) {
		error("ERROR: Unable to extract %s from %s\n", path, ipsw);
		free(path);
		free(blob);
		return -1;
	}

	img3 = img3_parse_file(data, size);
	if (img3 == NULL) {
		error("ERROR: Unable to parse IMG3: %s\n", path);
		free(data);
		free(path);
		free(blob);
		return -1;
	}
	if (data) {
		free(data);
		data = NULL;
	}

	if (idevicerestore_custom == 0) {
		if (img3_replace_signature(img3, blob) < 0) {
			error("ERROR: Unable to replace IMG3 signature\n");
			free(path);
			free(blob);
			return -1;
		}
	}

	if (img3_get_data(img3, &data, &size) < 0) {
		error("ERROR: Unable to reconstruct IMG3\n");
		img3_free(img3);
		free(path);
		return -1;
	}

	if (idevicerestore_debug) {
		char* out = strrchr(path, '/');
		if (out != NULL) {
			out++;
		} else {
			out = path;
		}
		write_file(out, data, size);
	}

	if (img3) {
		img3_free(img3);
		img3 = NULL;
	}
	if (blob) {
		free(blob);
		blob = NULL;
	}
	if (path) {
		free(path);
		path = NULL;
	}

	*pdata = data;
	*psize = size;
	return 0;
}

int get_signed_component_by_path(char* ipsw, plist_t tss, char* path, char** pdata, int* psize) {
	int size = 0;
	char* data = NULL;
	char* name = NULL;
	char* blob = NULL;
	img3_file* img3 = NULL;
	irecv_error_t error = 0;

	info("Extracting %s from TSS response\n", path);
	if (get_tss_data_by_path(tss, path, &name, &blob) < 0) {
		error("ERROR: Unable to get data for TSS %s entry\n", path);
		return -1;
	}

	info("Extracting %s from %s\n", path, ipsw);
	if (ipsw_extract_to_memory(ipsw, path, &data, &size) < 0) {
		error("ERROR: Unable to extract %s from %s\n", path, ipsw);
		free(path);
		free(blob);
		return -1;
	}

	img3 = img3_parse_file(data, size);
	if (img3 == NULL) {
		error("ERROR: Unable to parse IMG3: %s\n", path);
		free(data);
		free(path);
		free(blob);
		return -1;
	}
	if (data) {
		free(data);
		data = NULL;
	}

	if (idevicerestore_custom == 0) {
		if (img3_replace_signature(img3, blob) < 0) {
			error("ERROR: Unable to replace IMG3 signature\n");
			free(name);
			free(blob);
			return -1;
		}
	}

	if (img3_get_data(img3, &data, &size) < 0) {
		error("ERROR: Unable to reconstruct IMG3\n");
		img3_free(img3);
		free(name);
		return -1;
	}

	if (idevicerestore_debug) {
		char* out = strrchr(path, '/');
		if (out != NULL) {
			out++;
		} else {
			out = path;
		}
		write_file(out, data, size);
	}

	if (img3) {
		img3_free(img3);
		img3 = NULL;
	}
	if (blob) {
		free(blob);
		blob = NULL;
	}
	if (path) {
		free(name);
		name = NULL;
	}

	*pdata = data;
	*psize = size;
	return 0;
}

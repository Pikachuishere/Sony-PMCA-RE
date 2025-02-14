#include <cstring>
#include <fcntl.h>
#include <sys/mount.h>
#include <unistd.h>
#include <vector>

#include "api/android_data_backup.hpp"
#include "api/backup.hpp"
#include "api/bootloader.hpp"
#include "api/tweaks.hpp"
#include "api/usbcmd.hpp"
#include "usbshell.hpp"
#include "usbtransfer.hpp"

extern "C"
{
    #include "drivers/backup.h"
    #include "drivers/backup_senser.h"
    #include "mount.h"
    #include "process.h"
}

using namespace std;

#define USB_FEATURE_SHELL 0x23
#define USB_RESULT_SUCCESS 0
#define USB_RESULT_ERROR -1
#define USB_RESULT_ERROR_PROTECTION -2

const char *android_data_mount_dir = "/mnt";

struct usb_memory_read_request {
    unsigned int offset;
    unsigned int size;
};

struct usb_backup_read_request {
    int id;
};

struct usb_backup_write_request {
    int id;
    int size;
    char data[0xfff4];
};

struct usb_backup_protection_request {
    int enable;
};

struct usb_android_unmount_request {
    int commit_backup;
};

struct usb_shell_request {
    int cmd;
    char data[0xfff8];
};

struct usb_shell_response {
    int result;
};

static int get_file_size(const char *file)
{
    int fd = open(file, O_RDONLY);
    if (fd < 0)
        return -1;
    int size = lseek(fd, 0, SEEK_END);
    if (close(fd))
        return -1;
    return size;
}

void usbshell_loop()
{
    UsbCmd *cmd = new UsbCmd(USB_FEATURE_SHELL);
    UsbTransfer *transfer = new UsbSequenceTransfer(cmd);

    while (1) {
        usb_shell_request request;
        usb_shell_response response;
        transfer->read(&request, sizeof(request));

        if (request.cmd == *(int *) "TEST") {
            response.result = USB_RESULT_SUCCESS;
            transfer->write(&response, sizeof(response));
        } else if (request.cmd == *(int *) "SHEL") {
            int fd_stdin, fd_stdout;
            const char *args[] = { "sh", "-i", NULL };
            int pid = popen2((char *const *) args, &fd_stdin, &fd_stdout);
            response.result = pid >= 0 ? USB_RESULT_SUCCESS : pid;
            transfer->write(&response, sizeof(response));

            if (pid >= 0)
                usb_transfer_socket(transfer, fd_stdin, fd_stdout);
        } else if (request.cmd == *(int *) "PULL") {
            int size = get_file_size(request.data);
            int fd = size >= 0 ? open(request.data, O_RDONLY) : -1;
            response.result = fd >= 0 ? size : USB_RESULT_ERROR;
            transfer->write(&response, sizeof(response));

            if (fd >= 0)
                usb_transfer_read_fd(transfer, fd);
        } else if (request.cmd == *(int *) "PUSH") {
            int fd = open(request.data, O_WRONLY | O_SYNC | O_CREAT | O_TRUNC, 0755);
            response.result = fd >= 0 ? USB_RESULT_SUCCESS : fd;
            transfer->write(&response, sizeof(response));

            if (fd >= 0)
                usb_transfer_write_fd(transfer, fd);
        } else if (request.cmd == *(int *) "RMEM") {
            usb_memory_read_request *args = (usb_memory_read_request *) request.data;
            response.result = USB_RESULT_SUCCESS;
            transfer->write(&response, sizeof(response));
            usb_transfer_read_mem(transfer, args->offset, args->size);
        } else if (request.cmd == *(int *) "BLDR") {
            int fd = open(BOOTLOADER_DEV, O_RDONLY);
            vector<bootloader_block> blocks;
            try {
                blocks = bootloader_get_blocks(fd);
                response.result = blocks.size();
            } catch (const bootloader_error &) {
                response.result = USB_RESULT_ERROR;
            }
            transfer->write(&response, sizeof(response));

            for (vector<bootloader_block>::iterator it = blocks.begin(); it != blocks.end(); it++) {
                vector<char> data;
                try {
                    data = bootloader_read_block(fd, *it);
                } catch (const bootloader_error &) {
                    // ignore
                }
                usb_transfer_read_buffer(transfer, &data[0], data.size());
            }
            close(fd);
#ifdef API_backup
        } else if (request.cmd == *(int *) "BKRD") {
            usb_backup_read_request *args = (usb_backup_read_request *) request.data;
            BaseBackupProperty prop(args->id);
            try {
                vector<char> data = prop.read();
                response.result = data.size();
                transfer->write(&response, sizeof(response));
                transfer->read(NULL, 0);
                transfer->write(&data[0], data.size());
            } catch (...) {
                response.result = USB_RESULT_ERROR;
                transfer->write(&response, sizeof(response));
            }
        } else if (request.cmd == *(int *) "BKWR") {
            usb_backup_write_request *args = (usb_backup_write_request *) request.data;
            BaseBackupProperty prop(args->id);
            vector<char> data(args->data, args->data + args->size);
            try {
                prop.write(data);
                response.result = USB_RESULT_SUCCESS;
            } catch (const backup_protected_error &) {
                response.result = USB_RESULT_ERROR_PROTECTION;
            } catch (...) {
                response.result = USB_RESULT_ERROR;
            }
            transfer->write(&response, sizeof(response));
        } else if (request.cmd == *(int *) "BKSY") {
            Backup_sync_all();
            response.result = USB_RESULT_SUCCESS;
            transfer->write(&response, sizeof(response));
        } else if (request.cmd == *(int *) "BKST") {
            backup_senser_preset_data_status status;
            if (!backup_senser_cmd_preset_data_status(&status)) {
                response.result = sizeof(status);
                transfer->write(&response, sizeof(response));
                transfer->read(NULL, 0);
                transfer->write(&status, sizeof(status));
            } else {
                response.result = USB_RESULT_ERROR;
                transfer->write(&response, sizeof(response));
            }
        } else if (request.cmd == *(int *) "BKDA") {
            try {
                vector<char> data = Backup_read_data();
                response.result = data.size();
                transfer->write(&response, sizeof(response));
                usb_transfer_read_buffer(transfer, &data[0], data.size());
            } catch (...) {
                response.result = USB_RESULT_ERROR;
                transfer->write(&response, sizeof(response));
            }
        } else if (request.cmd == *(int *) "BKPR") {
            usb_backup_protection_request *args = (usb_backup_protection_request *) request.data;
            response.result = USB_RESULT_ERROR;
            try {
                if (tweak_protection().is_available()) {
                    tweak_protection().set_enabled(!args->enable);
                    response.result = USB_RESULT_SUCCESS;
                }
            } catch (...) {
                // ignore
            }
            transfer->write(&response, sizeof(response));
#endif
#ifdef API_android_data_backup
        } else if (request.cmd == *(int *) "AMNT") {
            try {
                mount_vfat(ANDROID_DATA_DEV, android_data_mount_dir);
                android_data_backup_mount(android_data_mount_dir);
                response.result = strlen(android_data_mount_dir);
                transfer->write(&response, sizeof(response));
                transfer->read(NULL, 0);
                transfer->write(android_data_mount_dir, strlen(android_data_mount_dir));
            } catch (...) {
                response.result = USB_RESULT_ERROR;
                transfer->write(&response, sizeof(response));
            }
        } else if (request.cmd == *(int *) "AUMT") {
            try {
                usb_android_unmount_request *args = (usb_android_unmount_request *) request.data;
                android_data_backup_unmount(android_data_mount_dir, args->commit_backup);
                umount(android_data_mount_dir);
                response.result = USB_RESULT_SUCCESS;
            } catch (...) {
                response.result = USB_RESULT_ERROR;
            }
            transfer->write(&response, sizeof(response));
#endif
        } else if (request.cmd == *(int *) "EXIT") {
            response.result = USB_RESULT_SUCCESS;
            transfer->write(&response, sizeof(response));
            break;
        } else {
            response.result = USB_RESULT_ERROR;
            transfer->write(&response, sizeof(response));
        }
    }

    usleep(500e3);
    delete transfer;
    delete cmd;
}

/****************************************************************************
 * WiiMC
 * Tantric 2009-2010
 *
 * fileop.cpp
 * File operations
 ***************************************************************************/

#include <gccore.h>
#include <errno.h>
#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#ifdef _USE_NTFS_
#include <ntfs.h>
#endif
#include <fat.h>
#ifdef _USE_EXT2_
#include <ext2.h>
#endif
#include <sdcard/wiisd_io.h>
#include <sdcard/gcsd.h>
#include <ogc/usbstorage.h>
#include <dirent.h>
#include <di/di.h>

#include "../xprintf.h"

#include "fileop.h"
#include "main.h"
#include "Tools/app_list.h"
#include "Tools/copy_app_in_category.h"
#include "Tools/throbber.h"
#include "DiskOperations/iso.h"
#include "DiskOperations/di2.h"

static const DISC_INTERFACE* sd = &__io_wiisd;
static const DISC_INTERFACE* usb = &__io_usbstorage;
#ifndef VWII
static const DISC_INTERFACE* dvd = &__io_wiidvd;
#endif

enum
{
	DEVICE_SD,
	DEVICE_USB

};

static char prefix[2][4] = { "sd", "usb" };

/****************************************************************************
 * FindPartitions
 *
 * Finds all partitions on the specified device
 ***************************************************************************/

#define le32_to_cpu(x) bswap32(x)

#define BYTES_PER_SECTOR 4096
#define NTFS_OEM_ID                         (0x4e54465320202020ULL)

#define PARTITION_TYPE_EMPTY                0x00 /* Empty */
#define PARTITION_TYPE_DOS33_EXTENDED       0x05 /* DOS 3.3+ extended partition */
#define PARTITION_TYPE_NTFS                 0x07 /* Windows NT NTFS */
#define PARTITION_TYPE_WIN95_EXTENDED       0x0F /* Windows 95 extended partition */

#define PARTITION_TYPE_LINUX		    0x83 /* GNU/Linux partition */
#define PARTITION_TYPE_LINUX_SWAP	    0x82 /* GNU/Linux Swap partition */
#define PARTITION_TYPE_LINUX_LVM	    0x8e /* GNU/Linux logical volume manager partition */
#define PARTITION_TYPE_LINUX_LUKS	    0xe8 /* GNU/Linux LUKS partition */

#define PARTITION_STATUS_NONBOOTABLE        0x00 /* Non-bootable */
#define PARTITION_STATUS_BOOTABLE           0x80 /* Bootable (active) */

#define MBR_SIGNATURE                       (0x55AA)
#define EBR_SIGNATURE                       (0x55AA)

#define BPB_FAT16_fileSysType  0x36
#define BPB_FAT32_fileSysType  0x52

#define T_FAT     1
#define T_NTFS    2
#define T_EXT2    3
#define T_ISO9660 4

static const char FAT_SIG[3] = {'F', 'A', 'T'};

/**
 * PRIMARY_PARTITION - Block device partition record
 */
typedef struct _PARTITION_RECORD {
    u8 status;                              /* Partition status; see above */
    u8 chs_start[3];                        /* Cylinder-head-sector address to first block of partition */
    u8 type;                                /* Partition type; see above */
    u8 chs_end[3];                          /* Cylinder-head-sector address to last block of partition */
    u32 lba_start;                          /* Local block address to first sector of partition */
    u32 block_count;                        /* Number of blocks in partition */
} __attribute__((__packed__)) PARTITION_RECORD;

/**
 * MASTER_BOOT_RECORD - Block device master boot record
 */
typedef struct _MASTER_BOOT_RECORD {
    u8 code_area[446];                      /* Code area; normally empty */
    PARTITION_RECORD partitions[4];         /* 4 primary partitions */
    u16 signature;                          /* MBR signature; 0xAA55 */
} __attribute__((__packed__)) MASTER_BOOT_RECORD;

/**
 * struct BIOS_PARAMETER_BLOCK - BIOS parameter block (bpb) structure.
 */
typedef struct {
	u16 bytes_per_sector;					/* Size of a sector in bytes. */
	u8  sectors_per_cluster;				/* Size of a cluster in sectors. */
	u16 reserved_sectors;					/* zero */
	u8  fats;								/* zero */
	u16 root_entries;						/* zero */
	u16 sectors;							/* zero */
	u8  media_type;							/* 0xf8 = hard disk */
	u16 sectors_per_fat;					/* zero */
	u16 sectors_per_track;					/* Required to boot Windows. */
	u16 heads;								/* Required to boot Windows. */
	u32 hidden_sectors;						/* Offset to the start of the partition */
	u32 large_sectors;						/* zero */
} __attribute__((__packed__)) BIOS_PARAMETER_BLOCK;

/**
 * struct NTFS_BOOT_SECTOR - NTFS boot sector structure.
 */
typedef struct {
	u8  jump[3];							/* Irrelevant (jump to boot up code).*/
	u64 oem_id;								/* Magic "NTFS    ". */
	BIOS_PARAMETER_BLOCK bpb;				/* See BIOS_PARAMETER_BLOCK. */
	u8 physical_drive;						/* 0x00 floppy, 0x80 hard disk */
	u8 current_head;						/* zero */
	u8 extended_boot_signature; 			/* 0x80 */
	u8 reserved2;							/* zero */
	s64 number_of_sectors;					/* Number of sectors in volume. */
	s64 mft_lcn;							/* Cluster location of mft data. */
	s64 mftmirr_lcn;						/* Cluster location of copy of mft. */
	s8  clusters_per_mft_record;			/* Mft record size in clusters. */
	u8  reserved0[3];						/* zero */
	s8  clusters_per_index_record;			/* Index block size in clusters. */
	u8  reserved1[3];						/* zero */
	u64 volume_serial_number;				/* Irrelevant (serial number). */
	u32 checksum;							/* Boot sector checksum. */
	u8  bootstrap[426];						/* Irrelevant (boot up code). */
	u16 end_of_sector_marker;				/* End of bootsector magic. */
} __attribute__((__packed__)) NTFS_BOOT_SECTOR;

/**
 * EXTENDED_PARTITION - Block device extended boot record
 */
typedef struct _EXTENDED_BOOT_RECORD {
    u8 code_area[446];                      /* Code area; normally empty */
    PARTITION_RECORD partition;             /* Primary partition */
    PARTITION_RECORD next_ebr;              /* Next extended boot record in the chain */
    u8 reserved[32];                        /* Normally empty */
    u16 signature;                          /* EBR signature; 0xAA55 */
} __attribute__((__packed__)) EXTENDED_BOOT_RECORD;

DEVICE_STRUCT part[2][MAX_DEVICES];

static void AddPartition(sec_t sector, int device, int type, int *devnum)
{
	if (*devnum >= MAX_DEVICES)
		return;

	DISC_INTERFACE *disc = (DISC_INTERFACE *)sd;

	if (device == DEVICE_USB)
	{
		disc = (DISC_INTERFACE *)usb;
	}

	char mount[10];
	sprintf(mount, "%s%i", prefix[device], *devnum+1);

	if(type == T_FAT)
	{
		if(!fatMount(mount, disc, sector, 8, 64))
			return;
		fatGetVolumeLabel(mount, part[device][*devnum].name);
	}
#ifdef _USE_NTFS_
	else if (type == T_NTFS)
	{
		if(!ntfsMount(mount, disc, sector, 8, 64, NTFS_DEFAULT | NTFS_RECOVER))
			return;

		const char *name = ntfsGetVolumeName(mount);

		if(name)
			strcpy(part[device][*devnum].name, name);
		else
			part[device][*devnum].name[0] = 0;
	}
#endif
#ifdef _USE_EXT2_
	else if (type == T_EXT2)
	{
		if(!ext2Mount(mount, disc, sector, 8, 64, EXT2_FLAG_64BITS | EXT2_FLAG_JOURNAL_DEV_OK))
			return;

		const char *name = ext2GetVolumeName(mount);

		if(name)
			strcpy(part[device][*devnum].name, name);
		else
			part[device][*devnum].name[0] = 0;
	}
#endif
#ifndef VWII
	else if (type == T_ISO9660)
	{

		if (!MountDVD())
			return;

		strcpy(part[device][*devnum].name, "DVD");

	}
#endif

	strcpy(part[device][*devnum].mount, mount);
	part[device][*devnum].type = type;
	part[device][*devnum].interface = disc;
	part[device][*devnum].sector = sector;
	++*devnum;
}

static int FindPartitions(int device)
{
	int i;
	int devnum = 0;

	// clear list
	for(i=0; i < MAX_DEVICES; i++)
	{
		part[device][i].name[0] = 0;
		part[device][i].mount[0] = 0;
		part[device][i].sector = 0;
		part[device][i].interface = NULL;
		part[device][i].type = 0;
	}

	DISC_INTERFACE *interface;

	if(device == DEVICE_SD)
		interface = (DISC_INTERFACE *)sd;
	else
		interface = (DISC_INTERFACE *)usb;

	MASTER_BOOT_RECORD mbr;
	PARTITION_RECORD *partition = NULL;
	devnum = 0;
	sec_t part_lba = 0;

	union
	{
		u8 buffer[BYTES_PER_SECTOR];
		MASTER_BOOT_RECORD mbr;
		EXTENDED_BOOT_RECORD ebr;
		NTFS_BOOT_SECTOR boot;
	} sector;

	// Read the first sector on the device
	if (!interface->readSectors(0, 1, &sector.buffer))
	{
		errno = EIO;
		return -1;
	}

	// If this is the devices master boot record
	xprintf("0x%x\n", sector.mbr.signature);
	if (sector.mbr.signature == MBR_SIGNATURE)
	{
		memcpy(&mbr, &sector, sizeof(MASTER_BOOT_RECORD));
		xprintf("Valid Master Boot Record found\n");

		// Search the partition table for all partitions (max. 4 primary partitions)
		for (i = 0; i < 4; i++)
		{
			partition = &mbr.partitions[i];
			part_lba = le32_to_cpu(mbr.partitions[i].lba_start);

			xprintf(
					"Partition %i: %s, sector %u, type 0x%x\n",
					i + 1,
					partition->status == PARTITION_STATUS_BOOTABLE ? "bootable (active)"
							: "non-bootable", part_lba, partition->type);

			// Figure out what type of partition this is
			switch (partition->type)
			{
				// NTFS partition
				case PARTITION_TYPE_NTFS:
				{
					xprintf("Partition %i: Claims to be NTFS\n", i + 1);

					// Read and validate the NTFS partition
					if (interface->readSectors(part_lba, 1, &sector))
					{
						xprintf("sector.boot.oem_id: 0x%x\n", sector.boot.oem_id);
						xprintf("NTFS_OEM_ID: 0x%x\n", NTFS_OEM_ID);
						if (sector.boot.oem_id == NTFS_OEM_ID)
						{
							xprintf("Partition %i: Valid NTFS boot sector found\n", i + 1);
							AddPartition(part_lba, device, T_NTFS, &devnum);
						}
						else
						{
							xprintf("Partition %i: Invalid NTFS boot sector, not actually NTFS\n", i + 1);
						}
					}

					break;
				}
				// DOS 3.3+ or Windows 95 extended partition
				case PARTITION_TYPE_DOS33_EXTENDED:
				case PARTITION_TYPE_WIN95_EXTENDED:
				{
					xprintf("Partition %i: Claims to be Extended\n", i + 1);

					// Walk the extended partition chain, finding all NTFS partitions within it
					sec_t ebr_lba = part_lba;
					sec_t next_erb_lba = 0;
					do
					{
						// Read and validate the extended boot record
						if (interface->readSectors(ebr_lba + next_erb_lba, 1, &sector))
						{
							if (sector.ebr.signature == EBR_SIGNATURE)
							{
								xprintf(
										"Logical Partition @ %d: %s type 0x%x\n",
										ebr_lba + next_erb_lba,
										sector.ebr.partition.status
												== PARTITION_STATUS_BOOTABLE ? "bootable (active)"
												: "non-bootable",
										sector.ebr.partition.type);

								// Get the start sector of the current partition
								// and the next extended boot record in the chain
								part_lba = ebr_lba + next_erb_lba
										+ le32_to_cpu(
												sector.ebr.partition.lba_start);
								next_erb_lba = le32_to_cpu(
										sector.ebr.next_ebr.lba_start);

								if(sector.ebr.partition.type==PARTITION_TYPE_LINUX)
								{
									xprintf("Partition : type EXT2/3/4 found\n");
									AddPartition(part_lba, device, T_EXT2, &devnum);
								}
								// Check if this partition has a valid NTFS boot record
								else if (interface->readSectors(part_lba, 1, &sector))
								{
									if (sector.boot.oem_id == NTFS_OEM_ID)
									{
										xprintf(
												"Logical Partition @ %d: Valid NTFS boot sector found\n",
												part_lba);
										if (sector.ebr.partition.type
												!= PARTITION_TYPE_NTFS)
										{
											xprintf(
													"Logical Partition @ %d: Is NTFS but type is 0x%x; 0x%x was expected\n",
													part_lba,
													sector.ebr.partition.type,
													PARTITION_TYPE_NTFS);
										}
										AddPartition(part_lba, device, T_NTFS, &devnum);
									}
									else if (!memcmp(sector.buffer
											+ BPB_FAT16_fileSysType, FAT_SIG,
											sizeof(FAT_SIG)) || !memcmp(
											sector.buffer
													+ BPB_FAT32_fileSysType,
											FAT_SIG, sizeof(FAT_SIG)))
									{
										xprintf("Partition : Valid FAT boot sector found\n");
										AddPartition(part_lba, device, T_FAT, &devnum);
									}
								}
							}
							else
							{
								next_erb_lba = 0;
							}
						}
					} while (next_erb_lba);
					break;
				}

				case PARTITION_TYPE_LINUX:
				{
					xprintf("Partition %i: Claims to be LINUX\n", i + 1);

					// Read and validate the EXT2 partition
					AddPartition(part_lba, device, T_EXT2, &devnum);
					break;
				}

				// Ignore empty partitions
				case PARTITION_TYPE_EMPTY:
					xprintf("Partition %i: Claims to be empty\n", i + 1);
				// Unknown or unsupported partition type
				default:
				{
					// Check if this partition has a valid NTFS boot record anyway,
					// it might be misrepresented due to a lazy partition editor
					if (interface->readSectors(part_lba, 1, &sector))
					{
						if (sector.boot.oem_id == NTFS_OEM_ID)
						{
							xprintf("Partition %i: Valid NTFS boot sector found\n",i + 1);
							if (partition->type != PARTITION_TYPE_NTFS)
							{
								xprintf(
										"Partition %i: Is NTFS but type is 0x%x; 0x%x was expected\n",
										i + 1, partition->type,
										PARTITION_TYPE_NTFS);
							}
							AddPartition(part_lba, device, T_NTFS, &devnum);
						}
						else if (!memcmp(sector.buffer + BPB_FAT16_fileSysType,
								FAT_SIG, sizeof(FAT_SIG)) || !memcmp(
								sector.buffer + BPB_FAT32_fileSysType, FAT_SIG,
								sizeof(FAT_SIG)))
						{
							xprintf("Partition : Valid FAT boot sector found\n");
							AddPartition(part_lba, device, T_FAT, &devnum);
						}
						else
						{
							xprintf("Trying : EXT partition\n");
							AddPartition(part_lba, device, T_EXT2, &devnum);
						}
					}
					break;
				}
			}
		}
	}
	if(devnum==0) // it is assumed this device has no master boot record or no partitions found
	{
		xprintf("No Master Boot Record was found or no partitions found!\n");

		// As a last-ditched effort, search the first 64 sectors of the device for stray NTFS/FAT partitions
		for (i = 0; i < 64; i++)
		{
			if (interface->readSectors(i, 1, &sector))
			{
				if (sector.boot.oem_id == NTFS_OEM_ID)
				{
					xprintf("Valid NTFS boot sector found at sector %d!\n", i);
					AddPartition(i, device, T_NTFS, &devnum);
					break;
				}
				else if (!memcmp(sector.buffer + BPB_FAT16_fileSysType, FAT_SIG,
						sizeof(FAT_SIG)) || !memcmp(sector.buffer
						+ BPB_FAT32_fileSysType, FAT_SIG, sizeof(FAT_SIG)))
				{
					xprintf("Partition : Valid FAT boot sector found\n");
					AddPartition(i, device, T_FAT, &devnum);
					break;
				}
				else
				{
					xprintf("Trying : EXT partition\n");
					AddPartition(part_lba, device, T_EXT2, &devnum);
				}
			}
		}
	}
	return devnum;
}

static void UnmountPartitions(int device)
{
	char mount[11];
	int i;

	for(i=0; i < MAX_DEVICES; i++)
	{
		if(part[device][i].type == T_FAT)
		{
			sprintf(mount, "VFAT: %s:", part[device][i].mount);
			fatUnmount(mount);
			break;
		}
#ifdef _USE_NTFS_
		else if(part[device][i].type == T_NTFS)
		{
			sprintf(mount, "NTFS: %s:", part[device][i].mount);
			ntfsUnmount(part[device][i].mount, false);
			break;
		}
#endif
#ifdef _USE_EXT2_
		else if(part[device][i].type == T_EXT2)
		{
			sprintf(mount, "EXT2: %s:", part[device][i].mount);
			ext2Unmount(part[device][i].mount);
			break;
		}
#endif
#ifdef VWII
		else if(part[device][i].type == T_ISO9660)
		{
			sprintf(mount, "ISO9660: %s:", part[device][i].mount);
			UnMountDVD();
			break;
		}
#endif

		part[device][i].name[0] = 0;
		part[device][i].mount[0] = 0;
		part[device][i].sector = 0;
		part[device][i].interface = NULL;
		part[device][i].type = 0;
	}

}

/****************************************************************************
 * MountPartitions
 *
 * Shuts down the device
 * Attempts to startup the device specified and mounts all partitions
 ***************************************************************************/

static bool MountPartitions(int device)
{
	const DISC_INTERFACE* disc = NULL;

	switch(device)
	{
		case DEVICE_SD:
			disc = sd;
			break;
		case DEVICE_USB:
			disc = usb;
			break;
		default:
			return false; // unknown device
	}

	if(FindPartitions(device) > 0)
		return true;

	return false;
}

void MountAllDevices()
{
	if(sd->startup() && sd->isInserted())
		MountPartitions(DEVICE_SD);

	usleep(250000); // 1/4 sec

	if(usb->startup() && usb->isInserted())
		MountPartitions(DEVICE_USB);
#ifndef VWII
	usleep(250000); // 1/4 sec

	if(dvd->startup() && dvd->isInserted())
		MountDVD();

	usleep(250000);

	if(__io_gcsda.startup() && __io_gcsda.isInserted())
		fatMountSimple("gca", &__io_gcsda);

	usleep(250000);

	if(__io_gcsdb.startup() && __io_gcsdb.isInserted())
		fatMountSimple("gcb", &__io_gcsdb);
#endif
}

bool MountDVDFS()
{
	bool devicemounted = ISO9660_Mount();

	/*if(!devicemounted)
		devicemounted = FST_Mount();
	if(!devicemounted)
		devicemounted = GCFST_Mount();*/

	return devicemounted;
}

void UnMountDVD()
{

	ISO9660_Unmount();
}

bool MountDVD()
{
	if(!DVD_Inserted())
		return false;

	char read_buffer[2048];
	if(DI2_ReadDVD(read_buffer, 1, 0) == 0)
		return true;

	UnMountDVD();
	DI2_Mount();

	time_t timer1, timer2;
	timer1 = time(0);

	while(DI2_GetStatus() & DVD_INIT)
	{
		timer2 = time(0);
		if(timer2-timer1 > 15)
			return false;

		usleep(5000);
	}

	return MountDVDFS();
}

bool DVD_Inserted()
{
	uint32_t cover = 0;
	DI2_GetCoverRegister(&cover);

	if(cover & DVD_COVER_DISC_INSERTED)
		return true;

	return false;
}

void UnmountAllDevices()
{
	UnmountPartitions(DEVICE_SD);
	UnmountPartitions(DEVICE_USB);
#ifndef VWII
	UnMountDVD();
	fatUnmount("gca:");
	fatUnmount("gcb:");
#endif
}

#ifndef VWII
bool GCA_Inserted()
{

	return __io_gcsda.isInserted();
}

void check_gca()
{
	if(Settings.gca_insert <= 0)
	{

		if(__io_gcsda.startup() && __io_gcsda.isInserted())		// wenn sd karte gefunden, neu einlesen
		{
			fatMountSimple("gca", &__io_gcsda);
			Settings.gca_insert = 2;
		}
	}
	else if(Settings.gca_insert == 1)
	{
		if(!__io_gcsda.isInserted())				// wenn sd karte nicht gefunden, beenden
		{
			fatUnmount("gca:");
			Settings.gca_insert = -1;
		}
	}
}

bool GCB_Inserted()
{
	return __io_gcsdb.isInserted();
}

void check_gcb()
{
	if(Settings.gcb_insert <= 0)
	{

		if(__io_gcsdb.startup() && __io_gcsdb.isInserted())		// wenn sd karte gefunden, neu einlesen
		{
			fatMountSimple("gcb", &__io_gcsdb);
			Settings.gcb_insert = 2;
		}
	}
	else if(Settings.gcb_insert == 1)
	{
		if(!__io_gcsdb.isInserted())				// wenn sd karte nicht gefunden, beenden
		{
			fatUnmount("gcb:");
			Settings.gcb_insert = -1;
		}
	}
}
#endif

bool SDCard_Inserted()
{
	return sd->isInserted();
}

void check_sd()
{
	if(Settings.sd_insert <= 0)
	{

		if(sd->startup() && sd->isInserted())		// wenn sd karte gefunden, neu einlesen
		{
			MountPartitions(DEVICE_SD);
			Settings.sd_insert = 2;
		}
	}
	else if(Settings.sd_insert == 1)
	{
		if(!SDCard_Inserted())				// wenn sd karte nicht gefunden, beenden
		{
			UnmountPartitions(DEVICE_SD);
			Settings.sd_insert = -1;
		}
	}
}

bool USBDevice_Inserted()
{
	return usb->isInserted();
}

void check_usb()
{
	if(Settings.usb_insert <= 0)
	{
		if(usb->startup() && usb->isInserted())		// wenn usb ger�t gefunden, neu einlesen
		{
			MountPartitions(DEVICE_USB);
			Settings.usb_insert = 2;
		}
	}
	else if(Settings.usb_insert == 1)
	{
		if(!USBDevice_Inserted())			// wenn usb ger�t nicht mehr gefunden, beenden
		{
			UnmountPartitions(DEVICE_USB);
			Settings.usb_insert = -1;
		}
	}
}

#ifndef VWII
void check_dvd()
{
	if(Settings.dvd_insert <= 0)
	{

		if(DVD_Inserted())		// wenn dvd gefunden, neu einlesen
		{
			MountDVD();
			Settings.dvd_insert = 2;
		}
	}
	else if(Settings.dvd_insert == 1)
	{
		if(!DVD_Inserted())				// wenn dvd nicht gefunden, beenden
		{
			UnMountDVD();
			Settings.dvd_insert = -1;
		}
	}
}
#endif

void check_device()
{
	ResumeThrobberThread();

	app_list();

	if(Settings.current_category != 0)
		copy_app_in_category();
	else
		copy_app_in_unassigned();

	if(Settings.sd_insert == 2)
		Settings.sd_insert = 1;
	else if(Settings.sd_insert == -1)
		Settings.sd_insert = 0;

	if(Settings.usb_insert == 2)
		Settings.usb_insert = 1;
	else if(Settings.usb_insert == -1)
		Settings.usb_insert = 0;
#ifndef VWII
	if(Settings.dvd_insert == 2)
		Settings.dvd_insert = 1;
	else if(Settings.dvd_insert == -1)
		Settings.dvd_insert = 0;

	if(Settings.gca_insert == 2)
		Settings.gca_insert = 1;
	else if(Settings.gca_insert == -1)
		Settings.gca_insert = 0;

	if(Settings.gcb_insert == 2)
		Settings.gcb_insert = 1;
	else if(Settings.gcb_insert == -1)
		Settings.gcb_insert = 0;
#endif
	HaltThrobberThread();
}

string check_path(string old_path)
{
	DIR *dirHandle;
	struct dirent * dirEntry;

	if(old_path.length() > 0 && old_path.substr(old_path.length() -1) != "/")
		old_path += "/";

	string new_path = old_path.substr(0, old_path.find(":/") +2);
	old_path.erase(0, old_path.find("/") +1);
	string search;

	while((signed)old_path.find("/") != -1)
	{
		search = old_path.substr(0, old_path.find("/"));
		dirHandle = opendir(new_path.c_str());
		if (dirHandle)
		{
			while (0 != (dirEntry = readdir(dirHandle)))
			{
				if(strcasecmp(dirEntry->d_name, search.c_str()) == 0)
				{
					search = dirEntry->d_name;
					break;
				}
			}
			new_path += search;
			new_path += "/";
			closedir(dirHandle);
		}

		old_path.erase(0, old_path.find("/") +1);
	}

	return new_path;
}

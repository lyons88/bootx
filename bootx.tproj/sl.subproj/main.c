/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 *  main.c - Main functions for BootX.
 *
 *  Copyright (c) 1998-2004 Apple Computer, Inc.
 *
 *  DRI: Josh de Cesare
 */


#include <sl.h>
#include "aes.h"

static void Start(void *unused1, void *unused2, ClientInterfacePtr ciPtr);
static void Main(ClientInterfacePtr ciPtr);
static long InitEverything(ClientInterfacePtr ciPtr);
static long DecodeKernel(void *binary);
static long SetUpBootArgs(void);
static long CallKernel(void);
static void FailToBoot(long num);
static long InitMemoryMap(void);
static long GetOFVersion(void);
static long TestForKey(long key);
static long GetBootPaths(void);
static long ReadBootPlist(char *devSpec);

const unsigned long StartTVector[2] = {(unsigned long)Start, 0};

char gStackBaseAddr[0x8000];

char *gVectorSaveAddr;
long gImageLastKernelAddr = 0;
long gImageFirstBootXAddr = kLoadAddr;
long gKernelEntryPoint;
long gDeviceTreeAddr;
long gDeviceTreeSize;
long gBootArgsAddr;
long gBootArgsSize;
long gSymbolTableAddr;
long gSymbolTableSize;

long gBootSourceNumber = -1;
long gBootSourceNumberMax;
long gBootMode = kBootModeNormal;
long gBootDeviceType;
long gBootFileType;
char gHaveKernelCache = 0;
char gBootDevice[256];
char gBootFile[256];
TagPtr gBootDict = NULL;
static char gBootKernelCacheFile[512];
static char gExtensionsSpec[4096];
static char gCacheNameAdler[64 + sizeof(gBootFile)];
static char *gPlatformName = gCacheNameAdler;

char gTempStr[4096];

long *gDeviceTreeMMTmp = 0;

long gOFVersion = 0;

char *gKeyMap;

long gRootAddrCells;
long gRootSizeCells;

CICell gChosenPH;
CICell gOptionsPH;
CICell gScreenPH;
CICell gMemoryMapPH;
CICell gStdOutPH;

CICell gMMUIH;
CICell gMemoryIH;
CICell gStdOutIH;
CICell gKeyboardIH;

static char gOFVectorSave[kVectorSize];
static unsigned long gOFMSRSave;
static unsigned long gOFSPRG0Save;
static unsigned long gOFSPRG1Save;
static unsigned long gOFSPRG2Save;
static unsigned long gOFSPRG3Save;

//int gDebugCount = 0;

// Private Functions

static void Start(void *unused1, void *unused2, ClientInterfacePtr ciPtr)
{
  long newSP;
  
  // Move the Stack to a chunk of the BSS
  newSP = (long)gStackBaseAddr + sizeof(gStackBaseAddr) - 0x100;
  __asm__ volatile("mr r1, %0" : : "r" (newSP));
  
  Main(ciPtr);
}



static void Main(ClientInterfacePtr ciPtr)
{
  long ret;
  int  trycache;
  long flags, cachetime, kerneltime, exttime = 0;
  void *binary = (void *)kLoadAddr;
  
  ret = InitEverything(ciPtr);
  if (ret != 0) Exit();


  // Get or infer the boot paths.
  ret = GetBootPaths();
  if (ret != 0) FailToBoot(1);
  
#if kFailToBoot
  DrawSplashScreen(0);
#endif
  
  while (ret == 0) {
    trycache = (0 == (gBootMode & kBootModeSafe))
	      && (gBootKernelCacheFile[0] != 0);
    
    if (trycache && (gBootFileType == kBlockDeviceType)) do {
      
      // if we haven't found the kernel yet, don't use the cache
      ret = GetFileInfo(NULL, gBootFile, &flags, &kerneltime);
      if ((ret != 0) || ((flags & kFileTypeMask) != kFileTypeFlat)) {
	trycache = 0;
	break;
      }
      ret = GetFileInfo(NULL, gBootKernelCacheFile, &flags, &cachetime);
      if ((ret != 0) || ((flags & kFileTypeMask) != kFileTypeFlat)
        || (cachetime < kerneltime)) {
	trycache = 0;
	break;
      }
      ret = GetFileInfo(gExtensionsSpec, "Extensions", &flags, &exttime);
      if ((ret == 0) && ((flags & kFileTypeMask) == kFileTypeDirectory)
	&& (cachetime < exttime)) {
	trycache = 0;
	break;
      }
      if (kerneltime > exttime)
	exttime = kerneltime;
      if (cachetime != (exttime + 1)) {
	trycache = 0;
	break;
      }
    } while (0);
    
    if (trycache) {
      ret = LoadFile(gBootKernelCacheFile);
      if (ret != -1) {
        ret = DecodeKernel(binary);
        if (ret != -1) break;
      }
    }
    ret = LoadThinFatFile(gBootFile, &binary);
    if (ret != -1) ret = DecodeKernel(binary);
    if (ret != -1) break;
    
    ret = GetBootPaths();
    if (ret != 0) FailToBoot(2);
  }
  
  if (ret != 0) FailToBoot(3);
  
  if (!gHaveKernelCache) {
    ret = LoadDrivers(gExtensionsSpec);
    if (ret != 0) FailToBoot(4);
  }
  
#if kFailToBoot
  DrawSplashScreen(1);
#endif
  
  ret = SetUpBootArgs();
  if (ret != 0) FailToBoot(5);
  
  ret = CallKernel();
  
  FailToBoot(6);
}

static uint32_t UnescapeData(const uint8_t * src, 
                             uint32_t  srcLen,
                             uint8_t * dst,
                             uint32_t  dstMaxLen)
{
  uint32_t cnt, cnt2, dstLen = 0;
  uint8_t  byte;

  for (cnt = 0; cnt < srcLen;) {
    byte = src[cnt++];
    if (byte == 0xFF) {
      byte = src[cnt++];
      cnt2 = byte & 0x7F;
      byte = (byte & 0x80) ? 0xFF : 0x00;
    } else
      cnt2 = 1;
    while (cnt2--) {
      if (dstLen >= dstMaxLen)
        return (-1);
      dst[dstLen++] = byte;
    }
  }
  return (dstLen);
}

static long InitEverything(ClientInterfacePtr ciPtr)
{
  long   ret, mem_base, mem_base2, size;
  CICell keyboardPH;
  char   name[32], securityMode[33];
  long length;
  char *compatible;
  
  // Init the OF Client Interface.
  ret = InitCI(ciPtr);
  if (ret != 0) return -1;
  
  // Get the OF Version
  gOFVersion = GetOFVersion();
  if (gOFVersion == 0) return -1;
  
  // Get the address and size cells for the root.
  GetProp(Peer(0), "#address-cells", (char *)&gRootAddrCells, 4);
  GetProp(Peer(0), "#size-cells", (char *)&gRootSizeCells, 4);
  if ((gRootAddrCells > 2) || (gRootAddrCells > 2)) return -1;
  
  // Init the SL Words package.
  ret = InitSLWords();
  if (ret != 0) return -1;
  
  // Get the phandle for /options
  gOptionsPH = FindDevice("/options");
  if (gOptionsPH == -1) return -1;
  
  // Get the phandle for /chosen
  gChosenPH = FindDevice("/chosen");
  if (gChosenPH == -1) return -1;
  
  // Init the Memory Map.
  ret = InitMemoryMap();
  if (ret != 0) return -1;
  
  // Get IHandles for the MMU and Memory
  size = GetProp(gChosenPH, "mmu", (char *)&gMMUIH, 4);
  if (size != 4) {
    printf("Failed to get the IH for the MMU.\n");
    return -1;
  }
  size = GetProp(gChosenPH, "memory", (char *)&gMemoryIH, 4);
  if (size != 4) {
    printf("Failed to get the IH for the Memory.\n");
    return -1;
  }
  
  // Get first element of root's compatible property.
  ret = GetPackageProperty(Peer(0), "compatible", &compatible, &length);
  if (ret != -1)
    strcpy(gPlatformName, compatible);
  
  // Get stdout's IH, so that the boot display can be found.
  ret = GetProp(gChosenPH, "stdout", (char *)&gStdOutIH, 4);
  if (ret == 4) gStdOutPH = InstanceToPackage(gStdOutIH);
  else gStdOutPH = gStdOutIH = 0;
  
  // Try to find the keyboard using chosen
  ret = GetProp(gChosenPH, "stdin", (char *)&gKeyboardIH, 4);
  if (ret != 4) gKeyboardIH = 0;
  else {
    keyboardPH = InstanceToPackage(gKeyboardIH);
    ret = GetProp(keyboardPH, "name", name, 31);
    if (ret != -1) {
      name[ret] = '\0';
      if (strcmp(name, "keyboard") && strcmp(name, "kbd")) gKeyboardIH = 0;
    } else gKeyboardIH = 0;
  }
  
  // Try to the find the keyboard using open if chosen did not work.
  if (gKeyboardIH == 0) gKeyboardIH = Open("keyboard");
  if (gKeyboardIH == 0) gKeyboardIH = Open("kbd");
  
  // Get the key map set up, and make it up to date.
  gKeyMap = InitKeyMap(gKeyboardIH);
  if (gKeyMap == NULL) return -1;
  UpdateKeyMap();
  
  // Test for Secure Boot Mode.
  size = GetProp(gOptionsPH, "security-mode", securityMode, 32);
  if (size != -1) {
    securityMode[size] = '\0';
    if (strcmp(securityMode, "none")) gBootMode |= kBootModeSecure;
  }
  
#if kFailToBoot
  // 'cmd-s' or 'cmd-v' is pressed set outputLevel to kOutputLevelFull
  if (((gBootMode & kBootModeSecure) == 0) && TestForKey(kCommandKey) &&
      (TestForKey('s') || TestForKey('v'))) {
    SetOutputLevel(kOutputLevelFull);
  } else {
    SetOutputLevel(kOutputLevelOff);
  }
#else
  SetOutputLevel(kOutputLevelFull);
#endif
  
  // printf now works.
  printf("\n\nMac OS X Loader\n");
  
  // Test for Safe Boot Mode; Shift and not Delete.
  if (((gBootMode & kBootModeSecure) == 0)
      && TestForKey(kShiftKey) && !TestForKey(kDeleteKey)) {
    gBootMode |= kBootModeSafe;
  }

  {
    // Claim memory for the FS Cache.
    if (Claim(kFSCacheAddr, kFSCacheSize, 0) == 0) {
      printf("Claim for fs cache failed.\n");
      return -1;
    }
    
    // Claim memory for malloc.
    if (Claim(kMallocAddr, kMallocSize, 0) == 0) {
      printf("Claim for malloc failed.\n");
      return -1;
    }
    malloc_init((char *)kMallocAddr, kMallocSize);
    
    // Claim memory for the Load Addr.
    mem_base = Claim(kLoadAddr, kLoadSize, 0);
    if (mem_base == 0) {
      printf("Claim for Load Area failed.\n");
      return -1;
    }
    
    // Claim the memory for the Image Addr
    if (gOFVersion >= kOFVersion3x) {
      mem_base = Claim(kImageAddr, kImageSize, 0);
      if (mem_base == 0) {
	printf("Claim for Image Area failed.\n");
	return -1;
      }
    } else {
      // Claim the 1:1 mapped chunks first.
      mem_base  = Claim(kImageAddr0, kImageSize0, 0);
      mem_base2 = Claim(kImageAddr2, kImageSize2, 0);
      if ((mem_base == 0) || (mem_base2 == 0)) {
        printf("Claim for Image Area failed.\n");
        return -1;
      }
      
      // Unmap the old xcoff stack.
      CallMethod(2, 0, gMMUIH, "unmap", 0x00380000, 0x00080000);
      
      // Grab the physical memory then the logical.
      CallMethod(3, 1, gMemoryIH, "claim",
              kImageAddr1Phys, kImageSize1, 0, &mem_base);
      CallMethod(3, 1, gMMUIH, "claim",
              kImageAddr1, kImageSize1, 0, &mem_base2);
      if ((mem_base == 0) || (mem_base2 == 0)) {
        printf("Claim for Image Area failed.\n");
        return -1;
      }
	
      // Map them together.
      CallMethod(4, 0, gMMUIH, "map",
		kImageAddr1Phys, kImageAddr1, kImageSize1, 0);
    }
    
    bzero((char *)kImageAddr, kImageSize);
    
    // Allocate some space for the Vector Save area.
    gVectorSaveAddr = AllocateBootXMemory(kVectorSize);
    if (gVectorSaveAddr == 0) {
      printf("Allocation for the Vector Save Area failed.\n");
      return -1;
    }
    // Find all the displays and set them up.
    ret = InitDisplays(1);
    if (ret != 0) {
      printf("InitDisplays failed.\n");
      return -1;
    }
  }  
  
  return 0;
}


long ThinFatBinary(void **binary, unsigned long *length)
{
  long ret;
  
  ret = ThinFatBinaryMachO(binary, length);
  if (ret == -1) ret = ThinFatBinaryElf(binary, length);
  
  return ret;
}

static long DecodeKernel(void *binary)
{
  long ret;
  compressed_kernel_header *kernel_header = (compressed_kernel_header *)binary;
  u_int32_t size;
  
  if (kernel_header->signature == 'comp') {
    if (kernel_header->compress_type != 'lzss')
      return -1;
    if (kernel_header->platform_name[0] && strcmp(gPlatformName, kernel_header->platform_name))
      return -1;
    if (kernel_header->root_path[0] && strcmp(gBootFile, kernel_header->root_path))
      return -1;
    
    binary = AllocateBootXMemory(kernel_header->uncompressed_size);
    
    size = decompress_lzss((u_int8_t *) binary, &kernel_header->data[0], kernel_header->compressed_size);
    if (kernel_header->uncompressed_size != size) {
      printf("size mismatch from lzss %x\n", size);
      return -1;
    }
    if (kernel_header->adler32 !=
	Adler32(binary, kernel_header->uncompressed_size)) {
      printf("adler mismatch\n");
      return -1;
    }
  }
  
  ThinFatBinary(&binary, 0);
  
  ret = DecodeMachO(binary);
  if (ret == -1) ret = DecodeElf(binary);
  
  return ret;
}


static long SetUpBootArgs(void)
{
  boot_args_ptr      args;
  CICell             memoryPH;
  long               graphicsBoot = 1;
  long               ret, cnt, size, dash;
  long               sKey, vKey, keyPos;
  char               ofBootArgs[240], *ofArgs, tc, keyStr[8];
  unsigned char      mem_regs[kMaxDRAMBanks*16];
  unsigned long      mem_banks, bank_shift;
  
  // Save file system cache statistics.
  SetProp(gChosenPH, "BootXCacheHits", (char *)&gCacheHits, 4);
  SetProp(gChosenPH, "BootXCacheMisses", (char *)&gCacheMisses, 4);
  SetProp(gChosenPH, "BootXCacheEvicts", (char *)&gCacheEvicts, 4);
  
  // Allocate some memory for the BootArgs.
  gBootArgsSize = sizeof(boot_args);
  gBootArgsAddr = AllocateKernelMemory(gBootArgsSize);
  
  // Add the BootArgs to the memory-map.
  AllocateMemoryRange("BootArgs", gBootArgsAddr, gBootArgsSize);
  
  args = (boot_args_ptr)gBootArgsAddr;
  
  args->Revision = kBootArgsRevision;
  args->Version = kBootArgsVersion1;
  args->machineType = 0;
  
  // Check the Keyboard for 'cmd-s' and 'cmd-v'
  UpdateKeyMap();
  if ((gBootMode & kBootModeSecure) == 0) {
    sKey = TestForKey(kCommandKey) && TestForKey('s');
    vKey = TestForKey(kCommandKey) && TestForKey('v');
  } else {
    sKey = 0;
    vKey = 0;
  }
  
  // if 'cmd-s' or 'cmd-v' was pressed do a text boot.
  if (sKey || vKey) graphicsBoot = 0;
  
  // Create the command line.
  if (gOFVersion < kOFVersion3x) {
    ofBootArgs[0] = ' ';
    size = GetProp(gChosenPH, "machargs", ofBootArgs + 1, (sizeof(ofBootArgs) - 2));
    if (size == -1) {
      size = GetProp(gOptionsPH, "boot-command", ofBootArgs, (sizeof(ofBootArgs) - 1));
      if (size == -1) ofBootArgs[0] = '\0';
      else ofBootArgs[size] = '\0';
      // Look for " bootr" but skip the number.
      if (!strncmp(ofBootArgs + 1, " bootr", 6)) {
	strcpy(ofBootArgs, ofBootArgs + 7);
      } else ofBootArgs[0] = '\0';
      SetProp(gChosenPH, "machargs", ofBootArgs, strlen(ofBootArgs) + 1);
    } else ofBootArgs[size] = '\0';
    // Force boot-command to start with 0 bootr.
    sprintf(gTempStr, "0 bootr%s", ofBootArgs);
    SetProp(gOptionsPH, "boot-command", gTempStr, strlen(gTempStr));
  } else {
    size = GetProp(gOptionsPH, "boot-args", ofBootArgs, (sizeof(ofBootArgs) - 1));
    if (size == -1) ofBootArgs[0] = '\0';
    else ofBootArgs[size] = '\0';
  }
  
  if (ofBootArgs[0] != '\0') {
    // Look for special options and copy the rest.
    dash = 0;
    ofArgs = ofBootArgs;
    while ((tc = *ofArgs) != '\0') { 
      tc = tolower(tc);
      
      // Check for entering a dash arg.
      if (tc == '-') {
	dash = 1;
	ofArgs++;
	continue;
      }
      
      // Do special stuff if in a dash arg.
      if (dash) {
	if        (tc == 's') {
	  graphicsBoot = 0;
	  ofArgs++;
	  sKey = 0;
	} else if (tc == 'v') {
	  graphicsBoot = 0;
	  ofArgs++;
	  vKey = 0;
	} else {
	  // Check for exiting dash arg
	  if (isspace(tc)) dash = 0;
	  
	  // Copy any non 's' or 'v'
	  ofArgs++;
	}
      } else {
	// Not a dash arg so just copy it.
	ofArgs++;
      }
    }
  }
  
  // Add any pressed keys (s, v, shift) to the command line
  keyPos = 0;
  if (sKey || vKey || (gBootMode & kBootModeSafe)) {
    keyStr[keyPos++] = '-';
    
    if (sKey) keyStr[keyPos++] = 's';
    if (vKey) keyStr[keyPos++] = 'v';
    if (gBootMode & kBootModeSafe) keyStr[keyPos++] = 'x';
    
    keyStr[keyPos++] = ' ';
  }
  keyStr[keyPos++] = '\0';
  
  sprintf(args->CommandLine, "%s%s", keyStr, ofBootArgs);
  
  // If the address or size cells are larger than 1, use page numbers
  // and signify Boot Args Version 2.
  if ((gRootAddrCells == 1) && (gRootSizeCells == 1)) bank_shift = 0;
  else {
    bank_shift = 12;
    args->Version = kBootArgsVersion2;
  }
  
  // Get the information about the memory banks
  memoryPH = FindDevice("/memory");
  if (memoryPH == -1) return -1;
  size = GetProp(memoryPH, "reg", mem_regs, kMaxDRAMBanks * 16);
  if (size == 0) return -1;
  mem_banks = size / (4 * (gRootAddrCells + gRootSizeCells));
  if (mem_banks > kMaxDRAMBanks) mem_banks = kMaxDRAMBanks;
  
  // Convert the reg properties to 32 bit values
  for (cnt = 0; cnt < mem_banks; cnt++) {
    if (gRootAddrCells == 1) {
      args->PhysicalDRAM[cnt].base =
	*(unsigned long *)(mem_regs + cnt * 4 * (gRootAddrCells + gRootSizeCells)) >> bank_shift;
    } else {
      args->PhysicalDRAM[cnt].base =
	*(unsigned long long *)(mem_regs + cnt * 4 * (gRootAddrCells + gRootSizeCells)) >> bank_shift;
      
    }
    
    if (gRootSizeCells == 1) {
      args->PhysicalDRAM[cnt].size =
	*(unsigned long *)(mem_regs + cnt * 4 * (gRootAddrCells + gRootSizeCells) + 4 * gRootAddrCells) >> bank_shift;
    } else {
      args->PhysicalDRAM[cnt].size =
	*(unsigned long long *)(mem_regs + cnt * 4 * (gRootAddrCells + gRootSizeCells) + 4 * gRootAddrCells) >> bank_shift;
      
    }
  }
  
  // Collapse the memory banks into contiguous chunks
  for (cnt = 0; cnt < mem_banks - 1; cnt++) {
    if ((args->PhysicalDRAM[cnt + 1].base != 0) &&
	((args->PhysicalDRAM[cnt].base + args->PhysicalDRAM[cnt].size) !=
	 args->PhysicalDRAM[cnt + 1].base)) continue;
    
    args->PhysicalDRAM[cnt].size += args->PhysicalDRAM[cnt + 1].size;
    bcopy(args->PhysicalDRAM + cnt + 2, args->PhysicalDRAM + cnt + 1, (mem_banks - cnt - 2) * sizeof(DRAMBank));
    mem_banks--;
    cnt--;
  }
  bzero(args->PhysicalDRAM + mem_banks, (kMaxDRAMBanks - mem_banks) * sizeof(DRAMBank));
  
  // Get the video info
  GetMainScreenPH(&args->Video, 1);
  args->Video.v_display = graphicsBoot;
  
  // Add the DeviceTree to the memory-map.
  // The actuall address and size must be filled in later.
  AllocateMemoryRange("DeviceTree", 0, 0);
  
  ret = FlattenDeviceTree();
  if (ret != 0) return -1;
  
  // Fill in the address and size of the device tree.
  if (gDeviceTreeAddr) {
    gDeviceTreeMMTmp[0] = gDeviceTreeAddr;
    gDeviceTreeMMTmp[1] = gDeviceTreeSize;
  }
  
  args->deviceTreeP = (void *)gDeviceTreeAddr;
  args->deviceTreeLength = gDeviceTreeSize;
  args->topOfKernelData = AllocateKernelMemory(0);
  
  return 0;
}


static long CallKernel(void)
{
  unsigned long msr, cnt;
  
  Quiesce();
  
  printf("\nCall Kernel!\n");
  
  // Save SPRs for OF
  __asm__ volatile("mfmsr %0" : "=r" (gOFMSRSave));
  __asm__ volatile("mfsprg %0, 0" : "=r" (gOFSPRG0Save));
  __asm__ volatile("mfsprg %0, 1" : "=r" (gOFSPRG1Save));
  __asm__ volatile("mfsprg %0, 2" : "=r" (gOFSPRG2Save));
  __asm__ volatile("mfsprg %0, 3" : "=r" (gOFSPRG3Save));
  
  // Turn off translations
  msr = 0x00001000;
  __asm__ volatile("sync");
  __asm__ volatile("mtmsr %0" : : "r" (msr));
  __asm__ volatile("isync");
  
  // Save the OF's Exceptions Vectors
  bcopy(0x0, gOFVectorSave, kVectorSize);
  
  // Move the Exception Vectors
  bcopy(gVectorSaveAddr, 0x0, kVectorSize);
  for (cnt = 0; cnt < kVectorSize; cnt += 0x20) {
    __asm__ volatile("dcbf 0, %0" : : "r" (cnt));
    __asm__ volatile("icbi 0, %0" : : "r" (cnt));
  }
  
  // Move the Image1 save area for OF 1.x / 2.x
  if (gOFVersion < kOFVersion3x) {
    bcopy((char *)kImageAddr1Phys, (char *)kImageAddr1, kImageSize1);
    for (cnt = kImageAddr1; cnt < kImageSize1; cnt += 0x20) {
      __asm__ volatile("dcbf 0, %0" : : "r" (cnt));
      __asm__ volatile("icbi 0, %0" : : "r" (cnt));
    }
  }
  
  // Make sure everything get sync'd up.
  __asm__ volatile("isync");
  __asm__ volatile("sync");
  __asm__ volatile("eieio");
  
  // Call the Kernel's entry point
  (*(void (*)())gKernelEntryPoint)(gBootArgsAddr, kMacOSXSignature);

  // Restore OF's Exception Vectors
  bcopy(gOFVectorSave, 0x0, 0x3000);
  for (cnt = 0; cnt < kVectorSize; cnt += 0x20) {
    __asm__ volatile("dcbf 0, %0" : : "r" (cnt));
    __asm__ volatile("icbi 0, %0" : : "r" (cnt));
  }
  
  // Restore SPRs for OF
  __asm__ volatile("mtsprg 0, %0" : : "r" (gOFSPRG0Save));
  __asm__ volatile("mtsprg 1, %0" : : "r" (gOFSPRG1Save));
  __asm__ volatile("mtsprg 2, %0" : : "r" (gOFSPRG2Save));
  __asm__ volatile("mtsprg 3, %0" : : "r" (gOFSPRG3Save));
  
  // Restore translations
  __asm__ volatile("sync");
  __asm__ volatile("mtmsr %0" : : "r" (gOFMSRSave));
  __asm__ volatile("isync");
  
  return -1;
}


static void FailToBoot(long num)
{
  // useful for those holding down command-v ...
  printf("FailToBoot: %d\n", num);
#if kFailToBoot
  DrawFailedBootPicture();
  while (1);
  num = 0;
#else
  Enter(); // For debugging
#endif
}


static long InitMemoryMap(void)
{
  long result;
  
  result = Interpret(0, 1,
		     " dev /chosen"
		     " new-device"
		     " \" memory-map\" device-name"
		     " active-package"
		     " device-end"
		     , &gMemoryMapPH);
  
  return result;
}


static long GetOFVersion(void)
{
  CICell ph;
  char   versStr[256], *tmpStr;
  long   vers, size;
  
  // Get the openprom package
  ph = FindDevice("/openprom");
  if (ph == -1) return 0;
  
  // Get it's model property
  size = GetProp(ph, "model", versStr, 255);
  if (size == -1) return -1;
  versStr[size] = '\0';
  
  // Find the start of the number.
  tmpStr = NULL;
  if (!strncmp(versStr, "Open Firmware, ", 15)) {
    tmpStr = versStr + 15;
  } else if (!strncmp(versStr, "OpenFirmware ", 13)) {
    tmpStr = versStr + 13;
  } else return -1;
  
  // Clasify by each instance as needed...
  switch (*tmpStr) {
  case '1' :
    vers = kOFVersion1x;
    break;
    
  case '2' :
    vers = kOFVersion2x;
    break;
    
  case '3' :
    vers = kOFVersion3x;
    break;
    
  case '4' :
    vers = kOFVersion4x;
    break;
    
  default :
    vers = 0;
    break;
  }

  return vers;
}


static long TestForKey(long key)
{
  long keyNum;
  long bp;
  char tc;
  
  if (gOFVersion < kOFVersion3x) {
    switch(key) {
    case 'a' :         keyNum =   7; break;
    case 's' :         keyNum =   6; break;
    case 'v' :         keyNum =  14; break;
    case 'y' :         keyNum =  23; break;
    case kCommandKey : keyNum =  48; break;
    case kOptKey     : keyNum =  61; break;
    case kShiftKey   : keyNum =  63; break;
    case kControlKey : keyNum =  49; break;
    default : keyNum = -1; break;
    }
  } else {
    switch(key) {
    case 'a' :         keyNum =   3; break;
    case 's' :         keyNum =  17; break;
    case 'v' :         keyNum =  30; break;
    case 'y' :         keyNum =  27; break;
    case kCommandKey : keyNum = 228; break;
    case kOptKey     : keyNum = 229; break;
    case kShiftKey   : keyNum = 230; break;
    case kControlKey : keyNum = 231; break;
    case kDeleteKey  : keyNum = 45; break;
    default : keyNum = -1; break;
    }
    
    // Map the right modifier keys on to the left.
    gKeyMap[28] |= gKeyMap[28] << 4;
  }
  
  if (keyNum == -1) return 0;
  
  bp = keyNum & 7;
  tc = gKeyMap[keyNum >> 3];
  
  return (tc & (1 << bp)) != 0;
}


#define kBootpBootFileOffset (108)

static long GetBootPaths(void)
{
  long ret, cnt, cnt2, cnt3, cnt4, size, partNum, bootplen, bsdplen;
  unsigned long adler32;
  char *filePath, *buffer, uuidStr[64];
  
  if (gBootSourceNumber == -1) {
    // Get the boot device and derive its type
    // (try chosen "bootpath", then boot-device in the options)
    size = GetProp(gChosenPH, "bootpath", gBootDevice, 255);
    gBootDevice[size] = '\0';
    if (gBootDevice[0] == '\0') {
      size = GetProp(gOptionsPH, "boot-device", gBootDevice, 255);
      gBootDevice[size] = '\0';
    }
// hardcode to my Apple_Boot before my Apple_RAID
// debug: override boot device to boot from disk even if OF used TFTP
//printf("old gBootDevice: %s\n", gBootDevice);
//strcpy(gBootDevice, "fw/node@d0010100007a9d/sbp-2@c000/@0:12");  // SmartDisk
//strcpy(gBootDevice, "fw/node@d04b491d060252/sbp-2@c000/@0:9");  // mconcatI
//strcpy(gBootDevice, "fw/node@d04b491d060252/sbp-2@c000/@0:11");  // mconcatI
//strcpy(gBootDevice, "fw/node@d04b491d075f57/sbp-2@c000/@0:2");  // mconcatII
//strcpy(gBootDevice, "fw/node@d04b491d075f57/sbp-2@c000/@0:4");  // mconcatII
//strcpy(gBootDevice, "fw/node@50770e0000676f/sbp-2@4000/@0:3");  // m120
//strcpy(gBootDevice, "fw/node@50770e0000725b/sbp-2@4000/@0:3");  // m120


    // Look for Boot.plist-based booter stuff (like RAID :)
    ret = ReadBootPlist(gBootDevice);
    if (ret == 0) {
      // success -> gBootDevice = "AppleRAID/#:0,\\\\:tbxi"
      (void)LookForRAID(gBootDict);	// could take gBootDevice?
    }
    

    // note RAID itself is of "block" type like members
    gBootDeviceType = GetDeviceType(gBootDevice);
    if(gBootDeviceType == -1) {
      printf("Could not find boot device %s\n", gBootDevice);
      return -1;
    }


    // Get the boot file (e.g. mach_kernel)
    size = GetProp(gChosenPH, "bootargs", gBootFile, 256);
    gBootFile[size] = '\0';
    
    if (gBootFile[0] != '\0') {
      gBootFileType = GetDeviceType(gBootFile);
      gBootSourceNumberMax = 0;
    } else {
      gBootSourceNumber = 0;
      gBootFileType = gBootDeviceType;
      if (gBootFileType == kNetworkDeviceType) gBootSourceNumberMax = 1;
      else {
	if (gOFVersion < kOFVersion3x) {
	  gBootSourceNumberMax = 4;
	} else {
	  gBootSourceNumberMax = 6;
	}
      }
    }
// gBootSourceNumberMax = 2;	// helpful to prevent lots of probing
    
    if (gBootFileType == kNetworkDeviceType) {
      SetProp(Peer(0), "net-boot", NULL, 0);
    }
  }
  
  if (gBootSourceNumber >= gBootSourceNumberMax) return -1;
  
  if (gBootSourceNumberMax != 0) {
    switch (gBootFileType) {
    case kNetworkDeviceType :
      // Find the end of the device spec.
      cnt = 0;
      while (gBootDevice[cnt] != ':') cnt++;
      
      // Copy the device spec with the ':'.
      strncpy(gBootFile, gBootDevice, cnt + 1);
      
      // Check for bootp-responce or bsdp-responce.
      bootplen = GetPropLen(gChosenPH, "bootp-response");
      bsdplen  = GetPropLen(gChosenPH, "bsdp-response");
      if ((bootplen > 0) || (bsdplen > 0)) {
	if (bootplen > 0) {
	  buffer = malloc(bootplen);
	  GetProp(gChosenPH, "bootp-response", buffer, bootplen);
	} else {
	  buffer = malloc(bsdplen);
	  GetProp(gChosenPH, "bsdp-response", buffer, bsdplen);
	}
	
	// Flip the slash's to back slash's while looking for the last one.
	cnt = cnt2 = kBootpBootFileOffset;
	while (buffer[cnt] != '\0') {
	  if (buffer[cnt] == '/') {
	    buffer[cnt] = '\\';
	    cnt2 = cnt + 1;
	  }
	  cnt++;
	}
	
	// Add a comma at the front.
	buffer[kBootpBootFileOffset - 1] = ',';
	
	// Append the the root dir to the device spec.
	strncat(gBootFile, buffer + kBootpBootFileOffset - 1,
		cnt2 - kBootpBootFileOffset + 1);
	
	free(buffer);
      } else {
	// Look for the start of the root dir path.
	cnt3 = cnt;
	while (gBootDevice[cnt3] != ',') cnt3++;
	
	// Find the end of the path.  Look for a comma or null.
	cnt2 = cnt3 + 1;
	while ((gBootDevice[cnt2] != '\0') && (gBootDevice[cnt2] != ',')) cnt2++;
	
	// Find the last back slash or comma in the path
	cnt4 = cnt2 - 1;
	while ((gBootDevice[cnt4] != ',') && (gBootDevice[cnt4] != '\\')) cnt4--;
	
	// Copy the IP addresses if needed.
	if (gOFVersion < kOFVersion3x) {
	  strncat(gBootFile, gBootDevice + cnt + 1, cnt3 - cnt - 1);
	}
	
	// Add on the directory path
	strncat(gBootFile, gBootDevice + cnt3, cnt4 - cnt3 + 1);
      }
      
      // Add on the kernel name
      strcat(gBootFile, "mach.macosx");
      
      // Add on postfix
      strcat(gBootFile, gBootDevice + cnt2);
      break;
      
    case kBlockDeviceType :
      // Find the first ':'.
      cnt = 0;
      while ((gBootDevice[cnt] != '\0') && (gBootDevice[cnt] != ':')) cnt++;
      if (gBootDevice[cnt] == '\0') return -1;
      
      // Find the comma after the ':'.
      cnt2 = cnt + 1;
      while ((gBootDevice[cnt2]  != '\0') && (gBootDevice[cnt] != ',')) cnt2++;
      
      // Get just the partition number
      strncpy(gBootFile, gBootDevice + cnt + 1, cnt2 - cnt - 1);
      partNum = strtol(gBootFile, 0, 10);
      if (partNum == 0) partNum = strtol(gBootFile, 0, 16);
      
      // Adjust the partition number.
      // Pass 0 & 1, no offset. Pass 2 & 3, offset 1, Pass 4 & 5, offset 2.
      partNum += gBootSourceNumber / 2;
      
      // Construct the boot-file
      strncpy(gBootFile, gBootDevice, cnt + 1);
      sprintf(gBootFile + cnt + 1, "%d,%s\\mach_kernel",
	      partNum, ((gBootSourceNumber & 1) ? "" : "\\"));
      
      // and the cache file name
      
      bzero(gCacheNameAdler + 64, sizeof(gBootFile));
      strcpy(gCacheNameAdler + 64, gBootFile);
      adler32 = Adler32(gCacheNameAdler, sizeof(gCacheNameAdler));
      
      strncpy(gBootKernelCacheFile, gBootDevice, cnt + 1);
      sprintf(gBootKernelCacheFile + cnt + 1, 
		"%d,\\System\\Library\\Caches\\com.apple.kernelcaches\\kernelcache.%08lX", partNum, adler32);
      break;
      
    default:
      printf("Failed to infer Boot Device Type.\n");
      return -1;
      break;
    }
  }
  
  // Figure out the root dir.
  ret = ConvertFileSpec(gBootFile, gExtensionsSpec, &filePath);
  if (ret == -1) {
    printf("Failed to determine root directory\n");
    return -1;
  }
  
  strcat(gExtensionsSpec, ",");
  
  // Add in any extra path to gRootDir.
  cnt = 0;
  while (filePath[cnt] != '\0') cnt++;
  
  if (cnt != 0) {
    for (cnt2 = cnt - 1; cnt2 >= 0; cnt2--) {
      if (filePath[cnt2] == '\\') {
	strncat(gExtensionsSpec, filePath, cnt2 + 1);
	break;
      }
    }
  }

  // Figure out the extensions dir.
  if (gBootFileType == kBlockDeviceType) {
    cnt = strlen(gExtensionsSpec);
    if ((cnt > 2) && (gExtensionsSpec[cnt-1] == '\\') && (gExtensionsSpec[cnt-2] == '\\'))
	cnt--;
    strcpy(gExtensionsSpec + cnt, "System\\Library\\");
  }

  // technically could just do this once at the end
  SetProp(gChosenPH, "rootpath", gBootFile, strlen(gBootFile) + 1);

  if (GetFSUUID(gBootFile, uuidStr) == 0) {
    printf("setting boot-uuid to: %s\n", uuidStr);
    SetProp(gChosenPH, "boot-uuid", uuidStr, strlen(uuidStr) + 1);
  }
  
  gBootSourceNumber++;
  
  return 0;
}

#define BOOTPLIST_PATH "com.apple.Boot.plist"

// ReadBootPlist could live elsewhere
static long ReadBootPlist(char *devSpec)
{
  char plistSpec[256];
  int len;

  do {
    // construct the Boot.plist spec
    if (ConvertFileSpec(devSpec, plistSpec, NULL))  break;
    strncat(plistSpec, ",\\\\", 255-strlen(plistSpec));
    strncat(plistSpec, BOOTPLIST_PATH, 255-strlen(plistSpec));

    // load the contents
    if ((len = LoadFile(plistSpec)) < 0)  break;
    // we could try for the root as well as the blessed folder
    *((char*)kLoadAddr + len) = '\0';  // terminate for parser safety

    if (ParseXML((char*)kLoadAddr, &gBootDict) < 0 || !gBootDict) {
      printf("couldn't parse %s\n", BOOTPLIST_PATH);
      break;
    }

    return 0;
  } while(0);

  return -1;
}

// Public Functions

long GetDeviceType(char *devSpec)
{
  CICell ph;
  long   size;
  char   deviceType[32];
  
  if (isRAIDPath(devSpec))
    return kBlockDeviceType;

  ph = FindDevice(devSpec);
  if (ph == -1) return -1;
  
  size = GetProp(ph, "device_type", deviceType, 31);
  if (size != -1) deviceType[size] = '\0';
  else deviceType[0] = '\0';
  
  if (strcmp(deviceType, "network") == 0) return kNetworkDeviceType;
  if (strcmp(deviceType, "block") == 0) return kBlockDeviceType;
  
  return kUnknownDeviceType;
}


long ConvertFileSpec(char *fileSpec, char *devSpec, char **filePath)
{
  long cnt;
  
  // Find the first ':' in the fileSpec.
  cnt = 0;
  while ((fileSpec[cnt] != '\0') && (fileSpec[cnt] != ':')) cnt++;
  if (fileSpec[cnt] == '\0') return -1;
  
  // Find the next ',' in the fileSpec.
  while ((fileSpec[cnt] != '\0') && (fileSpec[cnt] != ',')) cnt++;
  
  // Copy the string to devSpec.
  strncpy(devSpec, fileSpec, cnt);
  devSpec[cnt] = '\0';
  
  // If there is a filePath start it after the ',', otherwise NULL.
  if (filePath != NULL) {
    if (fileSpec[cnt] != '\0') {
      *filePath = fileSpec + cnt + 1;
    } else {
      *filePath = NULL;
    }
  }
  
  return 0;
}


long MatchThis(CICell phandle, char *string)
{
  long ret, length;
  char *name, *model, *compatible;
  
  ret = GetPackageProperty(phandle, "name", &name, &length);
  if ((ret == -1) || (length == 0)) name = NULL;
  
  ret = GetPackageProperty(phandle, "model", &model, &length);
  if ((ret == -1) || (length == 0)) model = NULL;
  
  ret = GetPackageProperty(phandle, "compatible", &compatible, &length);
  if ((ret == -1) || (length == 0)) model = NULL;
  
  if ((name != NULL) && strcmp(name, string) == 0) return 0;
  if ((model != NULL) && strcmp(model, string) == 0) return 0;
  
  if (compatible != NULL) {
    while (*compatible != '\0') { 
      if (strcmp(compatible, string) == 0) return 0;
      
      compatible += strlen(compatible) + 1;
    }
  }
  
  return -1;
}


void *AllocateBootXMemory(long size)
{
  long addr = gImageFirstBootXAddr - size;
  
  if (addr < gImageLastKernelAddr) return 0;
  
  gImageFirstBootXAddr = addr;
  
  return (void *)addr;
}


long AllocateKernelMemory(long size)
{
  long addr = gImageLastKernelAddr;
  
  gImageLastKernelAddr += (size + 0xFFF) & ~0xFFF;
  
  if (gImageLastKernelAddr > gImageFirstBootXAddr)
    FailToBoot(-1);
  
  return addr;
}


long AllocateMemoryRange(char *rangeName, long start, long length)
{
  long result, *buffer;
  
  buffer = AllocateBootXMemory(2 * sizeof(long));
  if (buffer == 0) return -1;
  
  buffer[0] = start;
  buffer[1] = length;
  
  result = SetProp(gMemoryMapPH, rangeName, (char *)buffer, 2 * sizeof(long));
  if (result == -1) return -1;
  
  return 0;
}

#define BASE 65521L /* largest prime smaller than 65536 */
#define NMAX 5000  
// NMAX (was 5521) the largest n such that 255n(n+1)/2 + (n+1)(BASE-1) <= 2^32-1

#define DO1(buf,i)  {s1 += buf[i]; s2 += s1;}
#define DO2(buf,i)  DO1(buf,i); DO1(buf,i+1);
#define DO4(buf,i)  DO2(buf,i); DO2(buf,i+2);
#define DO8(buf,i)  DO4(buf,i); DO4(buf,i+4);
#define DO16(buf)   DO8(buf,0); DO8(buf,8);

unsigned long Adler32(unsigned char *buf, long len)
{
    unsigned long s1 = 1; // adler & 0xffff;
    unsigned long s2 = 0; // (adler >> 16) & 0xffff;
    int k;

    while (len > 0) {
        k = len < NMAX ? len : NMAX;
        len -= k;
        while (k >= 16) {
            DO16(buf);
	    buf += 16;
            k -= 16;
        }
        if (k != 0) do {
            s1 += *buf++;
	    s2 += s1;
        } while (--k);
        s1 %= BASE;
        s2 %= BASE;
    }
    return (s2 << 16) | s1;
}

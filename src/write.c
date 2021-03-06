/* Copyright (c) Mark Harmstone 2016
 * 
 * This file is part of WinBtrfs.
 * 
 * WinBtrfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 * 
 * WinBtrfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with WinBtrfs.  If not, see <http://www.gnu.org/licenses/>. */

#include "btrfs_drv.h"

// BOOL did_split;
BOOL chunk_test = FALSE;

typedef struct {
    UINT64 start;
    UINT64 end;
    UINT8* data;
    UINT32 skip_start;
    UINT32 skip_end;
} write_stripe;

typedef struct {
    LONG stripes_left;
    KEVENT event;
} read_stripe_master;

typedef struct {
    PIRP Irp;
    PDEVICE_OBJECT devobj;
    IO_STATUS_BLOCK iosb;
    read_stripe_master* master;
} read_stripe;

// static BOOL extent_item_is_shared(EXTENT_ITEM* ei, ULONG len);
static NTSTATUS STDCALL write_data_completion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID conptr);
static void remove_fcb_extent(fcb* fcb, extent* ext, LIST_ENTRY* rollback);

BOOL find_address_in_chunk(device_extension* Vcb, chunk* c, UINT64 length, UINT64* address) {
    LIST_ENTRY* le;
    space* s;
    
    TRACE("(%p, %llx, %llx, %p)\n", Vcb, c->offset, length, address);
    
    if (IsListEmpty(&c->space_size))
        return FALSE;
    
    le = c->space_size.Flink;
    while (le != &c->space_size) {
        s = CONTAINING_RECORD(le, space, list_entry_size);
        
        if (s->size == length) {
            *address = s->address;
            return TRUE;
        } else if (s->size < length) {
            if (le == c->space_size.Flink)
                return FALSE;
            
            s = CONTAINING_RECORD(le->Blink, space, list_entry_size);
            
            *address = s->address;
            return TRUE;
        }
        
        le = le->Flink;
    }
    
    s = CONTAINING_RECORD(c->space_size.Blink, space, list_entry_size);
    
    if (s->size > length) {
        *address = s->address;
        return TRUE;
    }
    
    return FALSE;
}

chunk* get_chunk_from_address(device_extension* Vcb, UINT64 address) {
    LIST_ENTRY* le2;
    chunk* c;
    
    ExAcquireResourceSharedLite(&Vcb->chunk_lock, TRUE);
    
    le2 = Vcb->chunks.Flink;
    while (le2 != &Vcb->chunks) {
        c = CONTAINING_RECORD(le2, chunk, list_entry);
        
//         TRACE("chunk: %llx, %llx\n", c->offset, c->chunk_item->size);
        
        if (address >= c->offset && address < c->offset + c->chunk_item->size) {
            ExReleaseResourceLite(&Vcb->chunk_lock);
            return c;
        }
         
        le2 = le2->Flink;
    }
    
    ExReleaseResourceLite(&Vcb->chunk_lock);
    
    return NULL;
}

typedef struct {
    space* dh;
    device* device;
} stripe;

static UINT64 find_new_chunk_address(device_extension* Vcb, UINT64 size) {
    UINT64 lastaddr;
    LIST_ENTRY* le;
    
    lastaddr = 0;
    
    le = Vcb->chunks.Flink;
    while (le != &Vcb->chunks) {
        chunk* c = CONTAINING_RECORD(le, chunk, list_entry);
        
        if (c->offset >= lastaddr + size)
            return lastaddr;
        
        lastaddr = c->offset + c->chunk_item->size;
        
        le = le->Flink;
    }
    
    return lastaddr;
}

static BOOL find_new_dup_stripes(device_extension* Vcb, stripe* stripes, UINT64 max_stripe_size) {
    UINT64 j, devnum, devusage = 0xffffffffffffffff;
    space *devdh1 = NULL, *devdh2 = NULL;
    
    for (j = 0; j < Vcb->superblock.num_devices; j++) {
        UINT64 usage;
        
        usage = (Vcb->devices[j].devitem.bytes_used * 4096) / Vcb->devices[j].devitem.num_bytes;
        
        // favour devices which have been used the least
        if (usage < devusage) {
            if (!IsListEmpty(&Vcb->devices[j].space)) {
                LIST_ENTRY* le;
                space *dh1 = NULL, *dh2 = NULL;
                
                le = Vcb->devices[j].space.Flink;
                while (le != &Vcb->devices[j].space) {
                    space* dh = CONTAINING_RECORD(le, space, list_entry);
                    
                    if (dh->size >= max_stripe_size && (!dh1 || dh->size < dh1->size)) {
                        dh2 = dh1;
                        dh1 = dh;
                    }

                    le = le->Flink;
                }
                
                if (dh1 && (dh2 || dh1->size >= 2 * max_stripe_size)) {
                    devnum = j;
                    devusage = usage;
                    devdh1 = dh1;
                    devdh2 = dh2 ? dh2 : dh1;
                }
            }
        }
    }
    
    if (!devdh1)
        return FALSE;
    
    stripes[0].device = &Vcb->devices[devnum];
    stripes[0].dh = devdh1;
    stripes[1].device = stripes[0].device;
    stripes[1].dh = devdh2;
    
    return TRUE;
}

static BOOL find_new_stripe(device_extension* Vcb, stripe* stripes, UINT16 i, UINT64 max_stripe_size, UINT16 type) {
    UINT64 j, k, devnum = 0xffffffffffffffff, devusage = 0xffffffffffffffff;
    space* devdh = NULL;
    
    for (j = 0; j < Vcb->superblock.num_devices; j++) {
        UINT64 usage;
        BOOL skip = FALSE;
        
        // skip this device if it already has a stripe
        if (i > 0) {
            for (k = 0; k < i; k++) {
                if (stripes[k].device == &Vcb->devices[j]) {
                    skip = TRUE;
                    break;
                }
            }
        }
        
        if (!skip) {
            usage = (Vcb->devices[j].devitem.bytes_used * 4096) / Vcb->devices[j].devitem.num_bytes;
            
            // favour devices which have been used the least
            if (usage < devusage) {
                if (!IsListEmpty(&Vcb->devices[j].space)) {
                    LIST_ENTRY* le;
                    
                    le = Vcb->devices[j].space.Flink;
                    while (le != &Vcb->devices[j].space) {
                        space* dh = CONTAINING_RECORD(le, space, list_entry);
                        
                        if ((devnum != j && dh->size >= max_stripe_size) ||
                            (devnum == j && dh->size >= max_stripe_size && dh->size < devdh->size)
                        ) {
                            devdh = dh;
                            devnum = j;
                            devusage = usage;
                        }

                        le = le->Flink;
                    }
                }
            }
        }
    }
    
    if (!devdh)
        return FALSE;
    
    stripes[i].dh = devdh;
    stripes[i].device = &Vcb->devices[devnum];

    return TRUE;
}

chunk* alloc_chunk(device_extension* Vcb, UINT64 flags) {
    UINT64 max_stripe_size, max_chunk_size, stripe_size, stripe_length, factor;
    UINT64 total_size = 0, i, logaddr;
    UINT16 type, num_stripes, sub_stripes, max_stripes, min_stripes;
    stripe* stripes = NULL;
    ULONG cisize;
    CHUNK_ITEM_STRIPE* cis;
    chunk* c = NULL;
    space* s = NULL;
    BOOL success = FALSE;
    
    ExAcquireResourceExclusiveLite(&Vcb->chunk_lock, TRUE);
    
    for (i = 0; i < Vcb->superblock.num_devices; i++) {
        total_size += Vcb->devices[i].devitem.num_bytes;
    }
    TRACE("total_size = %llx\n", total_size);
    
    // We purposely check for DATA first - mixed blocks have the same size
    // as DATA ones.
    if (flags & BLOCK_FLAG_DATA) {
        max_stripe_size = 0x40000000; // 1 GB
        max_chunk_size = 10 * max_stripe_size;
    } else if (flags & BLOCK_FLAG_METADATA) {
        if (total_size > 0xC80000000) // 50 GB
            max_stripe_size = 0x40000000; // 1 GB
        else
            max_stripe_size = 0x10000000; // 256 MB
        
        max_chunk_size = max_stripe_size;
    } else if (flags & BLOCK_FLAG_SYSTEM) {
        max_stripe_size = 0x2000000; // 32 MB
        max_chunk_size = 2 * max_stripe_size;
    }
    
    max_chunk_size = min(max_chunk_size, total_size / 10); // cap at 10%
    
    TRACE("would allocate a new chunk of %llx bytes and stripe %llx\n", max_chunk_size, max_stripe_size);
    
    if (flags & BLOCK_FLAG_DUPLICATE) {
        min_stripes = 2;
        max_stripes = 2;
        sub_stripes = 0;
        type = BLOCK_FLAG_DUPLICATE;
    } else if (flags & BLOCK_FLAG_RAID0) {
        min_stripes = 2;
        max_stripes = Vcb->superblock.num_devices;
        sub_stripes = 0;
        type = BLOCK_FLAG_RAID0;
    } else if (flags & BLOCK_FLAG_RAID1) {
        min_stripes = 2;
        max_stripes = 2;
        sub_stripes = 1;
        type = BLOCK_FLAG_RAID1;
    } else if (flags & BLOCK_FLAG_RAID10) {
        min_stripes = 4;
        max_stripes = Vcb->superblock.num_devices;
        sub_stripes = 2;
        type = BLOCK_FLAG_RAID10;
    } else if (flags & BLOCK_FLAG_RAID5) {
        min_stripes = 3;
        max_stripes = Vcb->superblock.num_devices;
        sub_stripes = 1;
        type = BLOCK_FLAG_RAID5;
    } else if (flags & BLOCK_FLAG_RAID6) {
        FIXME("RAID6 not yet supported\n");
        goto end;
    } else { // SINGLE
        min_stripes = 1;
        max_stripes = 1;
        sub_stripes = 1;
        type = 0;
    }
    
    stripes = ExAllocatePoolWithTag(PagedPool, sizeof(stripe) * max_stripes, ALLOC_TAG);
    if (!stripes) {
        ERR("out of memory\n");
        goto end;
    }
    
    num_stripes = 0;
    
    if (type == BLOCK_FLAG_DUPLICATE) {
        if (!find_new_dup_stripes(Vcb, stripes, max_stripe_size))
            goto end;
        else
            num_stripes = max_stripes;
    } else {
        for (i = 0; i < max_stripes; i++) {
            if (!find_new_stripe(Vcb, stripes, i, max_stripe_size, type))
                break;
            else
                num_stripes++;
        }
    }
    
    // for RAID10, round down to an even number of stripes
    if (type == BLOCK_FLAG_RAID10 && (num_stripes % sub_stripes) != 0) {
        num_stripes -= num_stripes % sub_stripes;
    }
    
    if (num_stripes < min_stripes) {
        WARN("found %u stripes, needed at least %u\n", num_stripes, min_stripes);
        goto end;
    }
    
    c = ExAllocatePoolWithTag(NonPagedPool, sizeof(chunk), ALLOC_TAG);
    if (!c) {
        ERR("out of memory\n");
        goto end;
    }
    
    cisize = sizeof(CHUNK_ITEM) + (num_stripes * sizeof(CHUNK_ITEM_STRIPE));
    c->chunk_item = ExAllocatePoolWithTag(NonPagedPool, cisize, ALLOC_TAG);
    if (!c->chunk_item) {
        ERR("out of memory\n");
        goto end;
    }
    
    stripe_length = 0x10000; // FIXME? BTRFS_STRIPE_LEN in kernel
    
    stripe_size = max_stripe_size;
    for (i = 0; i < num_stripes; i++) {
        if (stripes[i].dh->size < stripe_size)
            stripe_size = stripes[i].dh->size;
    }
    
    if (type == 0 || type == BLOCK_FLAG_DUPLICATE || type == BLOCK_FLAG_RAID1)
        factor = 1;
    else if (type == BLOCK_FLAG_RAID0)
        factor = num_stripes;
    else if (type == BLOCK_FLAG_RAID10)
        factor = num_stripes / sub_stripes;
    else if (type == BLOCK_FLAG_RAID5)
        factor = num_stripes - 1;
    
    if (stripe_size * factor > max_chunk_size)
        stripe_size = max_chunk_size / factor;
    
    if (stripe_size % stripe_length > 0)
        stripe_size -= stripe_size % stripe_length;
    
    if (stripe_size == 0)
        goto end;
    
    c->chunk_item->size = stripe_size * factor;
    c->chunk_item->root_id = Vcb->extent_root->id;
    c->chunk_item->stripe_length = stripe_length;
    c->chunk_item->type = flags;
    c->chunk_item->opt_io_alignment = c->chunk_item->stripe_length;
    c->chunk_item->opt_io_width = c->chunk_item->stripe_length;
    c->chunk_item->sector_size = stripes[0].device->devitem.minimal_io_size;
    c->chunk_item->num_stripes = num_stripes;
    c->chunk_item->sub_stripes = sub_stripes;
    
    c->devices = ExAllocatePoolWithTag(NonPagedPool, sizeof(device*) * num_stripes, ALLOC_TAG);
    if (!c->devices) {
        ERR("out of memory\n");
        goto end;
    }

    cis = (CHUNK_ITEM_STRIPE*)&c->chunk_item[1];
    for (i = 0; i < num_stripes; i++) {
        cis[i].dev_id = stripes[i].device->devitem.dev_id;
        
        if (type == BLOCK_FLAG_DUPLICATE && i == 1 && stripes[i].dh == stripes[0].dh)
            cis[i].offset = stripes[0].dh->address + stripe_size;
        else
            cis[i].offset = stripes[i].dh->address;
        
        cis[i].dev_uuid = stripes[i].device->devitem.device_uuid;
        
        c->devices[i] = stripes[i].device;
    }
    
    logaddr = find_new_chunk_address(Vcb, c->chunk_item->size);
    
    Vcb->superblock.chunk_root_generation = Vcb->superblock.generation;
    
    c->size = cisize;
    c->offset = logaddr;
    c->used = c->oldused = 0;
    c->cache = NULL;
    InitializeListHead(&c->space);
    InitializeListHead(&c->space_size);
    InitializeListHead(&c->deleting);
    InitializeListHead(&c->changed_extents);
    
    ExInitializeResourceLite(&c->lock);
    ExInitializeResourceLite(&c->changed_extents_lock);
    
    s = ExAllocatePoolWithTag(NonPagedPool, sizeof(space), ALLOC_TAG);
    if (!s) {
        ERR("out of memory\n");
        goto end;
    }
    
    s->address = c->offset;
    s->size = c->chunk_item->size;
    InsertTailList(&c->space, &s->list_entry);
    InsertTailList(&c->space_size, &s->list_entry_size);
    
    protect_superblocks(Vcb, c);
    
    for (i = 0; i < num_stripes; i++) {
        stripes[i].device->devitem.bytes_used += stripe_size;
        
        space_list_subtract2(&stripes[i].device->space, NULL, cis[i].offset, stripe_size, NULL);
    }
    
    success = TRUE;
    
end:
    if (stripes)
        ExFreePool(stripes);
    
    if (!success) {
        if (c && c->chunk_item) ExFreePool(c->chunk_item);
        if (c) ExFreePool(c);
        if (s) ExFreePool(s);
    } else {
        LIST_ENTRY* le;
        BOOL done = FALSE;
        
        le = Vcb->chunks.Flink;
        while (le != &Vcb->chunks) {
            chunk* c2 = CONTAINING_RECORD(le, chunk, list_entry);
            
            if (c2->offset > c->offset) {
                InsertHeadList(le->Blink, &c->list_entry);
                done = TRUE;
                break;
            }
            
            le = le->Flink;
        }
        
        if (!done)
            InsertTailList(&Vcb->chunks, &c->list_entry);
        
        c->created = TRUE;
        InsertTailList(&Vcb->chunks_changed, &c->list_entry_changed);
    }
    
    ExReleaseResourceLite(&Vcb->chunk_lock);

    return success ? c : NULL;
}

static NTSTATUS prepare_raid0_write(chunk* c, UINT64 address, void* data, UINT32 length, write_stripe* stripes) {
    UINT64 startoff, endoff;
    UINT16 startoffstripe, endoffstripe, stripenum;
    UINT64 pos, *stripeoff;
    UINT32 i;
    
    stripeoff = ExAllocatePoolWithTag(PagedPool, sizeof(UINT64) * c->chunk_item->num_stripes, ALLOC_TAG);
    if (!stripeoff) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    get_raid0_offset(address - c->offset, c->chunk_item->stripe_length, c->chunk_item->num_stripes, &startoff, &startoffstripe);
    get_raid0_offset(address + length - c->offset - 1, c->chunk_item->stripe_length, c->chunk_item->num_stripes, &endoff, &endoffstripe);
    
    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        if (startoffstripe > i) {
            stripes[i].start = startoff - (startoff % c->chunk_item->stripe_length) + c->chunk_item->stripe_length;
        } else if (startoffstripe == i) {
            stripes[i].start = startoff;
        } else {
            stripes[i].start = startoff - (startoff % c->chunk_item->stripe_length);
        }
        
        if (endoffstripe > i) {
            stripes[i].end = endoff - (endoff % c->chunk_item->stripe_length) + c->chunk_item->stripe_length;
        } else if (endoffstripe == i) {
            stripes[i].end = endoff + 1;
        } else {
            stripes[i].end = endoff - (endoff % c->chunk_item->stripe_length);
        }
        
        if (stripes[i].start != stripes[i].end) {
            stripes[i].data = ExAllocatePoolWithTag(NonPagedPool, stripes[i].end - stripes[i].start, ALLOC_TAG);
            
            if (!stripes[i].data) {
                ERR("out of memory\n");
                ExFreePool(stripeoff);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
        }
    }
    
    pos = 0;
    RtlZeroMemory(stripeoff, sizeof(UINT64) * c->chunk_item->num_stripes);
    
    stripenum = startoffstripe;
    while (pos < length) {
        if (pos == 0) {
            UINT32 writelen = min(stripes[stripenum].end - stripes[stripenum].start,
                                  c->chunk_item->stripe_length - (stripes[stripenum].start % c->chunk_item->stripe_length));
            
            RtlCopyMemory(stripes[stripenum].data, data, writelen);
            stripeoff[stripenum] += writelen;
            pos += writelen;
        } else if (length - pos < c->chunk_item->stripe_length) {
            RtlCopyMemory(stripes[stripenum].data + stripeoff[stripenum], (UINT8*)data + pos, length - pos);
            break;
        } else {
            RtlCopyMemory(stripes[stripenum].data + stripeoff[stripenum], (UINT8*)data + pos, c->chunk_item->stripe_length);
            stripeoff[stripenum] += c->chunk_item->stripe_length;
            pos += c->chunk_item->stripe_length;
        }
        
        stripenum = (stripenum + 1) % c->chunk_item->num_stripes;
    }

    ExFreePool(stripeoff);
    
    return STATUS_SUCCESS;
}

static NTSTATUS prepare_raid10_write(chunk* c, UINT64 address, void* data, UINT32 length, write_stripe* stripes) {
    UINT64 startoff, endoff;
    UINT16 startoffstripe, endoffstripe, stripenum;
    UINT64 pos, *stripeoff;
    UINT32 i;

    stripeoff = ExAllocatePoolWithTag(PagedPool, sizeof(UINT64) * c->chunk_item->num_stripes / c->chunk_item->sub_stripes, ALLOC_TAG);
    if (!stripeoff) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    get_raid0_offset(address - c->offset, c->chunk_item->stripe_length, c->chunk_item->num_stripes / c->chunk_item->sub_stripes, &startoff, &startoffstripe);
    get_raid0_offset(address + length - c->offset - 1, c->chunk_item->stripe_length, c->chunk_item->num_stripes / c->chunk_item->sub_stripes, &endoff, &endoffstripe);

    startoffstripe *= c->chunk_item->sub_stripes;
    endoffstripe *= c->chunk_item->sub_stripes;

    for (i = 0; i < c->chunk_item->num_stripes; i += c->chunk_item->sub_stripes) {
        UINT16 j;
        
        if (startoffstripe > i) {
            stripes[i].start = startoff - (startoff % c->chunk_item->stripe_length) + c->chunk_item->stripe_length;
        } else if (startoffstripe == i) {
            stripes[i].start = startoff;
        } else {
            stripes[i].start = startoff - (startoff % c->chunk_item->stripe_length);
        }
        
        if (endoffstripe > i) {
            stripes[i].end = endoff - (endoff % c->chunk_item->stripe_length) + c->chunk_item->stripe_length;
        } else if (endoffstripe == i) {
            stripes[i].end = endoff + 1;
        } else {
            stripes[i].end = endoff - (endoff % c->chunk_item->stripe_length);
        }
        
        if (stripes[i].start != stripes[i].end) {
            stripes[i].data = ExAllocatePoolWithTag(NonPagedPool, stripes[i].end - stripes[i].start, ALLOC_TAG);
            
            if (!stripes[i].data) {
                ERR("out of memory\n");
                ExFreePool(stripeoff);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        
        for (j = 1; j < c->chunk_item->sub_stripes; j++) {
            stripes[i+j].start = stripes[i].start;
            stripes[i+j].end = stripes[i].end;
            stripes[i+j].data = stripes[i].data;
        }
    }

    pos = 0;
    RtlZeroMemory(stripeoff, sizeof(UINT64) * c->chunk_item->num_stripes / c->chunk_item->sub_stripes);

    stripenum = startoffstripe / c->chunk_item->sub_stripes;
    while (pos < length) {
        if (pos == 0) {
            UINT32 writelen = min(stripes[stripenum * c->chunk_item->sub_stripes].end - stripes[stripenum * c->chunk_item->sub_stripes].start,
                                  c->chunk_item->stripe_length - (stripes[stripenum * c->chunk_item->sub_stripes].start % c->chunk_item->stripe_length));
            
            RtlCopyMemory(stripes[stripenum * c->chunk_item->sub_stripes].data, data, writelen);
            stripeoff[stripenum] += writelen;
            pos += writelen;
        } else if (length - pos < c->chunk_item->stripe_length) {
            RtlCopyMemory(stripes[stripenum * c->chunk_item->sub_stripes].data + stripeoff[stripenum], (UINT8*)data + pos, length - pos);
            break;
        } else {
            RtlCopyMemory(stripes[stripenum * c->chunk_item->sub_stripes].data + stripeoff[stripenum], (UINT8*)data + pos, c->chunk_item->stripe_length);
            stripeoff[stripenum] += c->chunk_item->stripe_length;
            pos += c->chunk_item->stripe_length;
        }
        
        stripenum = (stripenum + 1) % (c->chunk_item->num_stripes / c->chunk_item->sub_stripes);
    }

    ExFreePool(stripeoff);
    
    return STATUS_SUCCESS;
}

static NTSTATUS STDCALL read_stripe_completion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID ptr) {
    read_stripe* stripe = ptr;
    read_stripe_master* master = stripe->master;
    ULONG stripes_left = InterlockedDecrement(&master->stripes_left);
    
    stripe->iosb = Irp->IoStatus;
    
    if (stripes_left == 0)
        KeSetEvent(&master->event, 0, FALSE);
    
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS make_read_irp(PIRP old_irp, read_stripe* stripe, UINT64 offset, void* data, UINT32 length) {
    PIO_STACK_LOCATION IrpSp;
    PIRP Irp;
    
    if (!old_irp) {
        Irp = IoAllocateIrp(stripe->devobj->StackSize, FALSE);
        
        if (!Irp) {
            ERR("IoAllocateIrp failed\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    } else {
        Irp = IoMakeAssociatedIrp(old_irp, stripe->devobj->StackSize);
        
        if (!Irp) {
            ERR("IoMakeAssociatedIrp failed\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    
    IrpSp = IoGetNextIrpStackLocation(Irp);
    IrpSp->MajorFunction = IRP_MJ_READ;
    
    if (stripe->devobj->Flags & DO_BUFFERED_IO) {
        FIXME("FIXME - buffered IO\n");
        IoFreeIrp(Irp);
        return STATUS_INTERNAL_ERROR;
    } else if (stripe->devobj->Flags & DO_DIRECT_IO) {
        Irp->MdlAddress = IoAllocateMdl(data, length, FALSE, FALSE, NULL);
        if (!Irp->MdlAddress) {
            ERR("IoAllocateMdl failed\n");
            IoFreeIrp(Irp);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        MmProbeAndLockPages(Irp->MdlAddress, KernelMode, IoWriteAccess);
    } else {
        Irp->UserBuffer = data;
    }

    IrpSp->Parameters.Read.Length = length;
    IrpSp->Parameters.Read.ByteOffset.QuadPart = offset;
    
    Irp->UserIosb = &stripe->iosb;
    
    IoSetCompletionRoutine(Irp, read_stripe_completion, stripe, TRUE, TRUE, TRUE);
    
    stripe->Irp = Irp;
    
    return STATUS_SUCCESS;
}

static NTSTATUS prepare_raid5_write(PIRP Irp, chunk* c, UINT64 address, void* data, UINT32 length, write_stripe* stripes) {
    UINT64 startoff, endoff;
    UINT16 startoffstripe, endoffstripe, stripenum, parity;
    UINT64 start = 0xffffffffffffffff, end = 0;
    UINT64 pos, stripepos;
    UINT32 firststripesize, laststripesize;
    UINT32 i;
    UINT8* data2 = (UINT8*)data;
    UINT32 num_reads;
    BOOL same_stripe = FALSE;
    
    get_raid0_offset(address - c->offset, c->chunk_item->stripe_length, c->chunk_item->num_stripes - 1, &startoff, &startoffstripe);
    get_raid0_offset(address + length - c->offset - 1, c->chunk_item->stripe_length, c->chunk_item->num_stripes - 1, &endoff, &endoffstripe);
    
    for (i = 0; i < c->chunk_item->num_stripes - 1; i++) {
        UINT64 ststart, stend;
        
        if (startoffstripe > i) {
            ststart = startoff - (startoff % c->chunk_item->stripe_length) + c->chunk_item->stripe_length;
        } else if (startoffstripe == i) {
            ststart = startoff;
        } else {
            ststart = startoff - (startoff % c->chunk_item->stripe_length);
        }

        if (endoffstripe > i) {
            stend = endoff - (endoff % c->chunk_item->stripe_length) + c->chunk_item->stripe_length;
        } else if (endoffstripe == i) {
            stend = endoff + 1;
        } else {
            stend = endoff - (endoff % c->chunk_item->stripe_length);
        }

        if (ststart != stend) {
            stripes[i].start = ststart;
            stripes[i].end = stend;
            
            if (ststart < start) {
                start = ststart;
                firststripesize = c->chunk_item->stripe_length - (ststart % c->chunk_item->stripe_length);
            }

            if (stend > end) {
                end = stend;
                laststripesize = stend % c->chunk_item->stripe_length;
                if (laststripesize == 0)
                    laststripesize = c->chunk_item->stripe_length;
            }
        }
    }
    
    if (start == end) {
        ERR("error: start == end (%llx)\n", start);
        return STATUS_INTERNAL_ERROR;
    }
    
    if (startoffstripe == endoffstripe && start / c->chunk_item->stripe_length == end / c->chunk_item->stripe_length) {
        firststripesize = end - start;
        laststripesize = firststripesize;
    }

    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        stripes[i].data = ExAllocatePoolWithTag(NonPagedPool, end - start, ALLOC_TAG);
        if (!stripes[i].data) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        if (i < c->chunk_item->num_stripes - 1) {
            if (stripes[i].start == 0 && stripes[i].end == 0)
                stripes[i].start = stripes[i].end = start;
        }
    }
    
    num_reads = 0;
    
    for (i = 0; i < c->chunk_item->num_stripes - 1; i++) {
        if (stripes[i].start > start) {
            num_reads++;
        }
        
        if (stripes[i].end < end) {
            num_reads++;
        }
    }
    
    if (num_reads > 0) {
        UINT32 j;
        read_stripe_master* master;
        read_stripe* read_stripes;
        CHUNK_ITEM_STRIPE* cis = (CHUNK_ITEM_STRIPE*)&c->chunk_item[1];
        NTSTATUS Status;
        
        master = ExAllocatePoolWithTag(NonPagedPool, sizeof(read_stripe_master), ALLOC_TAG);
        if (!master) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        read_stripes = ExAllocatePoolWithTag(NonPagedPool, sizeof(read_stripe) * num_reads, ALLOC_TAG);
        if (!read_stripes) {
            ERR("out of memory\n");
            ExFreePool(master);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        parity = (((address - c->offset) / ((c->chunk_item->num_stripes - 1) * c->chunk_item->stripe_length)) + c->chunk_item->num_stripes - 1) % c->chunk_item->num_stripes;
        stripenum = (parity + 1) % c->chunk_item->num_stripes;
        
        j = 0;
        for (i = 0; i < c->chunk_item->num_stripes - 1; i++) {
            if (stripes[i].start > start) {
                read_stripes[j].Irp = NULL;
                read_stripes[j].devobj = c->devices[stripenum]->devobj;
                read_stripes[j].master = master;
                
                Status = make_read_irp(Irp, &read_stripes[j], start + cis[stripenum].offset, stripes[stripenum].data, stripes[i].start - start);
                if (!NT_SUCCESS(Status)) {
                    ERR("make_read_irp returned %08x\n", Status);
                    j++;
                    goto readend;
                }
                
                stripes[stripenum].skip_start = stripes[i].start - start;
                
                j++;
                if (j == num_reads) break;
            }
            
            stripenum = (stripenum + 1) % c->chunk_item->num_stripes;
        }
        
        if (j < num_reads) {
            parity = (((address + length - 1 - c->offset) / ((c->chunk_item->num_stripes - 1) * c->chunk_item->stripe_length)) + c->chunk_item->num_stripes - 1) % c->chunk_item->num_stripes;
            stripenum = (parity + 1) % c->chunk_item->num_stripes;
            
            for (i = 0; i < c->chunk_item->num_stripes - 1; i++) {
                if (stripes[i].end < end) {
                    read_stripes[j].Irp = NULL;
                    read_stripes[j].devobj = c->devices[stripenum]->devobj;
                    read_stripes[j].master = master;
                    
                    Status = make_read_irp(Irp, &read_stripes[j], stripes[i].end + cis[stripenum].offset, &stripes[stripenum].data[stripes[i].end - start], end - stripes[i].end);
                    if (!NT_SUCCESS(Status)) {
                        ERR("make_read_irp returned %08x\n", Status);
                        j++;
                        goto readend;
                    }
                    
                    stripes[stripenum].skip_end = end - stripes[i].end;
                    
                    j++;
                    if (j == num_reads) break;
                }
                
                stripenum = (stripenum + 1) % c->chunk_item->num_stripes;
            }
        }
        
        master->stripes_left = j;
        KeInitializeEvent(&master->event, NotificationEvent, FALSE);
        
        for (i = 0; i < j; i++) {
            Status = IoCallDriver(read_stripes[i].devobj, read_stripes[i].Irp);
            if (!NT_SUCCESS(Status)) {
                ERR("IoCallDriver returned %08x\n", Status);
                goto readend;
            }
        }
        
        KeWaitForSingleObject(&master->event, Executive, KernelMode, FALSE, NULL);
        
        for (i = 0; i < j; i++) {
            if (!NT_SUCCESS(read_stripes[i].iosb.Status)) {
                Status = read_stripes[i].iosb.Status;
                goto readend;
            }
        }
        
        Status = STATUS_SUCCESS;

readend:
        for (i = 0; i < j; i++) {
            if (read_stripes[i].Irp) {
                if (read_stripes[i].devobj->Flags & DO_DIRECT_IO) {
                    MmUnlockPages(read_stripes[i].Irp->MdlAddress);
                    IoFreeMdl(read_stripes[i].Irp->MdlAddress);
                }
                
                IoFreeIrp(read_stripes[i].Irp); // FIXME - what if IoCallDriver fails and other Irps are still running?
            }
        }
        
        ExFreePool(read_stripes);
        ExFreePool(master);
        
        if (!NT_SUCCESS(Status))
            return Status;
    }
    
    pos = 0;
    
    parity = (((address - c->offset) / ((c->chunk_item->num_stripes - 1) * c->chunk_item->stripe_length)) + c->chunk_item->num_stripes - 1) % c->chunk_item->num_stripes;
    stripepos = 0;
    
    if ((address - c->offset) % (c->chunk_item->stripe_length * (c->chunk_item->num_stripes - 1)) > 0) {
        UINT16 firstdata;
        BOOL first = TRUE;
        
        stripenum = (parity + 1) % c->chunk_item->num_stripes;
        
        for (i = 0; i < c->chunk_item->num_stripes - 1; i++) {
            ULONG copylen;
            
            if (pos >= length)
                break;
            
            if (stripes[i].start < start + firststripesize && stripes[i].start != stripes[i].end) {
                copylen = min(start + firststripesize - stripes[i].start, length - pos);
                
                if (!first && copylen < c->chunk_item->stripe_length) {
                    same_stripe = TRUE;
                    break;
                }

                RtlCopyMemory(&stripes[stripenum].data[firststripesize - copylen], &data2[pos], copylen);
                
                pos += copylen;
                first = FALSE;
            }
            
            stripenum = (stripenum + 1) % c->chunk_item->num_stripes;
        }
        
        firstdata = parity == 0 ? 1 : 0;
        
        RtlCopyMemory(stripes[parity].data, stripes[firstdata].data, firststripesize);
        
        for (i = firstdata + 1; i < c->chunk_item->num_stripes; i++) {
            if (i != parity)
                do_xor(&stripes[parity].data[0], &stripes[i].data[0], firststripesize);
        }
        
        if (!same_stripe) {
            stripepos = firststripesize;
            parity = (parity + 1) % c->chunk_item->num_stripes;
        }
    }
    
    while (length >= pos + c->chunk_item->stripe_length * (c->chunk_item->num_stripes - 1)) {
        UINT16 firstdata;
        
        stripenum = (parity + 1) % c->chunk_item->num_stripes;
        
        for (i = 0; i < c->chunk_item->num_stripes - 1; i++) {
            RtlCopyMemory(&stripes[stripenum].data[stripepos], &data2[pos], c->chunk_item->stripe_length);
            
            pos += c->chunk_item->stripe_length;
            stripenum = (stripenum +1) % c->chunk_item->num_stripes;
        }
        
        firstdata = parity == 0 ? 1 : 0;
        
        RtlCopyMemory(&stripes[parity].data[stripepos], &stripes[firstdata].data[stripepos], c->chunk_item->stripe_length);
        
        for (i = firstdata + 1; i < c->chunk_item->num_stripes; i++) {
            if (i != parity)
                do_xor(&stripes[parity].data[stripepos], &stripes[i].data[stripepos], c->chunk_item->stripe_length);
        }
        
        parity = (parity + 1) % c->chunk_item->num_stripes;
        stripepos += c->chunk_item->stripe_length;
    }
    
    if (pos < length) {
        UINT16 firstdata;
        
        if (!same_stripe)
            stripenum = (parity + 1) % c->chunk_item->num_stripes;
        
        i = 0;
        while (pos < length) {
            ULONG copylen;
            
            copylen = min(stripes[i].end - start - stripepos, length - pos);

            RtlCopyMemory(&stripes[stripenum].data[stripepos], &data2[pos], copylen);
            
            pos += copylen;
            stripenum = (stripenum + 1) % c->chunk_item->num_stripes;
            i++;
        }
        
        firstdata = parity == 0 ? 1 : 0;
        
        RtlCopyMemory(&stripes[parity].data[stripepos], &stripes[firstdata].data[stripepos], laststripesize);
        
        for (i = firstdata + 1; i < c->chunk_item->num_stripes; i++) {
            if (i != parity)
                do_xor(&stripes[parity].data[stripepos], &stripes[i].data[stripepos], laststripesize);
        }
    }
    
    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        stripes[i].start = start;
        stripes[i].end = end;
    }
    
    return STATUS_SUCCESS;
}

NTSTATUS STDCALL write_data(device_extension* Vcb, UINT64 address, void* data, BOOL need_free, UINT32 length, write_data_context* wtc, PIRP Irp, chunk* c) {
    NTSTATUS Status;
    UINT32 i;
    CHUNK_ITEM_STRIPE* cis;
    write_data_stripe* stripe;
    write_stripe* stripes = NULL;
    BOOL need_free2;
    
    TRACE("(%p, %llx, %p, %x)\n", Vcb, address, data, length);
    
    if (!c) {
        c = get_chunk_from_address(Vcb, address);
        if (!c) {
            ERR("could not get chunk for address %llx\n", address);
            return STATUS_INTERNAL_ERROR;
        }
    }
    
    if (c->chunk_item->type & BLOCK_FLAG_RAID6) {
        FIXME("RAID6 not yet supported\n");
        return STATUS_NOT_IMPLEMENTED;
    }
    
    stripes = ExAllocatePoolWithTag(PagedPool, sizeof(write_stripe) * c->chunk_item->num_stripes, ALLOC_TAG);
    if (!stripes) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    RtlZeroMemory(stripes, sizeof(write_stripe) * c->chunk_item->num_stripes);
    
    cis = (CHUNK_ITEM_STRIPE*)&c->chunk_item[1];
    
    if (c->chunk_item->type & BLOCK_FLAG_RAID0) {
        Status = prepare_raid0_write(c, address, data, length, stripes);
        if (!NT_SUCCESS(Status)) {
            ERR("prepare_raid0_write returned %08x\n", Status);
            ExFreePool(stripes);
            return Status;
        }
        
        if (need_free)
            ExFreePool(data);

        need_free2 = TRUE;
    } else if (c->chunk_item->type & BLOCK_FLAG_RAID10) {
        Status = prepare_raid10_write(c, address, data, length, stripes);
        if (!NT_SUCCESS(Status)) {
            ERR("prepare_raid10_write returned %08x\n", Status);
            ExFreePool(stripes);
            return Status;
        }
        
        if (need_free)
            ExFreePool(data);

        need_free2 = TRUE;
    } else if (c->chunk_item->type & BLOCK_FLAG_RAID5) {
        Status = prepare_raid5_write(Irp, c, address, data, length, stripes);
        if (!NT_SUCCESS(Status)) {
            ERR("prepare_raid5_write returned %08x\n", Status);
            ExFreePool(stripes);
            return Status;
        }
        
        if (need_free)
            ExFreePool(data);

        need_free2 = TRUE;
    } else {  // write same data to every location - SINGLE, DUP, RAID1
        for (i = 0; i < c->chunk_item->num_stripes; i++) {
            stripes[i].start = address - c->offset;
            stripes[i].end = stripes[i].start + length;
            stripes[i].data = data;
        }
        need_free2 = need_free;
    }

    for (i = 0; i < c->chunk_item->num_stripes; i++) {
        PIO_STACK_LOCATION IrpSp;
        
        // FIXME - handle missing devices
        
        stripe = ExAllocatePoolWithTag(NonPagedPool, sizeof(write_data_stripe), ALLOC_TAG);
        if (!stripe) {
            ERR("out of memory\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }
        
        if (stripes[i].start + stripes[i].skip_start == stripes[i].end - stripes[i].skip_end) {
            stripe->status = WriteDataStatus_Ignore;
            stripe->Irp = NULL;
            stripe->buf = stripes[i].data;
            stripe->need_free = need_free2;
        } else {
            stripe->context = (struct _write_data_context*)wtc;
            stripe->buf = stripes[i].data;
            stripe->need_free = need_free2;
            stripe->device = c->devices[i];
            RtlZeroMemory(&stripe->iosb, sizeof(IO_STATUS_BLOCK));
            stripe->status = WriteDataStatus_Pending;
            
            if (!Irp) {
                stripe->Irp = IoAllocateIrp(stripe->device->devobj->StackSize, FALSE);
            
                if (!stripe->Irp) {
                    ERR("IoAllocateIrp failed\n");
                    Status = STATUS_INTERNAL_ERROR;
                    goto end;
                }
            } else {
                stripe->Irp = IoMakeAssociatedIrp(Irp, stripe->device->devobj->StackSize);
                
                if (!stripe->Irp) {
                    ERR("IoMakeAssociatedIrp failed\n");
                    Status = STATUS_INTERNAL_ERROR;
                    goto end;
                }
            }
            
            IrpSp = IoGetNextIrpStackLocation(stripe->Irp);
            IrpSp->MajorFunction = IRP_MJ_WRITE;
            
            if (stripe->device->devobj->Flags & DO_BUFFERED_IO) {
                stripe->Irp->AssociatedIrp.SystemBuffer = stripes[i].data + stripes[i].skip_start;

                stripe->Irp->Flags = IRP_BUFFERED_IO;
            } else if (stripe->device->devobj->Flags & DO_DIRECT_IO) {
                stripe->Irp->MdlAddress = IoAllocateMdl(stripes[i].data + stripes[i].skip_start,
                                                        stripes[i].end - stripes[i].start - stripes[i].skip_start - stripes[i].skip_end, FALSE, FALSE, NULL);
                if (!stripe->Irp->MdlAddress) {
                    ERR("IoAllocateMdl failed\n");
                    Status = STATUS_INTERNAL_ERROR;
                    goto end;
                }
                
                MmProbeAndLockPages(stripe->Irp->MdlAddress, KernelMode, IoWriteAccess);
            } else {
                stripe->Irp->UserBuffer = stripes[i].data + stripes[i].skip_start;
            }

            IrpSp->Parameters.Write.Length = stripes[i].end - stripes[i].start - stripes[i].skip_start - stripes[i].skip_end;
            IrpSp->Parameters.Write.ByteOffset.QuadPart = stripes[i].start + cis[i].offset + stripes[i].skip_start;
            
            stripe->Irp->UserIosb = &stripe->iosb;
            wtc->stripes_left++;

            IoSetCompletionRoutine(stripe->Irp, write_data_completion, stripe, TRUE, TRUE, TRUE);
        }

        InsertTailList(&wtc->stripes, &stripe->list_entry);
    }
    
    Status = STATUS_SUCCESS;
    
end:

    if (stripes) ExFreePool(stripes);
    
    if (!NT_SUCCESS(Status)) {
        free_write_data_stripes(wtc);
        ExFreePool(wtc);
    }
    
    return Status;
}

NTSTATUS STDCALL write_data_complete(device_extension* Vcb, UINT64 address, void* data, UINT32 length, PIRP Irp, chunk* c) {
    write_data_context* wtc;
    NTSTATUS Status;
// #ifdef DEBUG_PARANOID
//     UINT8* buf2;
// #endif
    
    wtc = ExAllocatePoolWithTag(NonPagedPool, sizeof(write_data_context), ALLOC_TAG);
    if (!wtc) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeInitializeEvent(&wtc->Event, NotificationEvent, FALSE);
    InitializeListHead(&wtc->stripes);
    wtc->tree = FALSE;
    wtc->stripes_left = 0;
    
    Status = write_data(Vcb, address, data, FALSE, length, wtc, Irp, c);
    if (!NT_SUCCESS(Status)) {
        ERR("write_data returned %08x\n", Status);
        free_write_data_stripes(wtc);
        ExFreePool(wtc);
        return Status;
    }
    
    if (wtc->stripes.Flink != &wtc->stripes) {
        // launch writes and wait
        LIST_ENTRY* le = wtc->stripes.Flink;
        while (le != &wtc->stripes) {
            write_data_stripe* stripe = CONTAINING_RECORD(le, write_data_stripe, list_entry);
            
            if (stripe->status != WriteDataStatus_Ignore)
                IoCallDriver(stripe->device->devobj, stripe->Irp);
            
            le = le->Flink;
        }
        
        KeWaitForSingleObject(&wtc->Event, Executive, KernelMode, FALSE, NULL);
        
        le = wtc->stripes.Flink;
        while (le != &wtc->stripes) {
            write_data_stripe* stripe = CONTAINING_RECORD(le, write_data_stripe, list_entry);
            
            if (stripe->status != WriteDataStatus_Ignore && !NT_SUCCESS(stripe->iosb.Status)) {
                Status = stripe->iosb.Status;
                break;
            }
            
            le = le->Flink;
        }
        
        free_write_data_stripes(wtc);
    }

    ExFreePool(wtc);

// #ifdef DEBUG_PARANOID
//     buf2 = ExAllocatePoolWithTag(NonPagedPool, length, ALLOC_TAG);
//     Status = read_data(Vcb, address, length, NULL, FALSE, buf2, NULL, Irp);
//     
//     if (!NT_SUCCESS(Status) || RtlCompareMemory(buf2, data, length) != length)
//         int3;
//     
//     ExFreePool(buf2);
// #endif

    return STATUS_SUCCESS;
}

static NTSTATUS STDCALL write_data_completion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID conptr) {
    write_data_stripe* stripe = conptr;
    write_data_context* context = (write_data_context*)stripe->context;
    LIST_ENTRY* le;
    
    // FIXME - we need a lock here
    
    if (stripe->status == WriteDataStatus_Cancelling) {
        stripe->status = WriteDataStatus_Cancelled;
        goto end;
    }
    
    stripe->iosb = Irp->IoStatus;
    
    if (NT_SUCCESS(Irp->IoStatus.Status)) {
        stripe->status = WriteDataStatus_Success;
    } else {
        le = context->stripes.Flink;
        
        stripe->status = WriteDataStatus_Error;
        
        while (le != &context->stripes) {
            write_data_stripe* s2 = CONTAINING_RECORD(le, write_data_stripe, list_entry);
            
            if (s2->status == WriteDataStatus_Pending) {
                s2->status = WriteDataStatus_Cancelling;
                IoCancelIrp(s2->Irp);
            }
            
            le = le->Flink;
        }
    }
    
end:
    if (InterlockedDecrement(&context->stripes_left) == 0)
        KeSetEvent(&context->Event, 0, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

void free_write_data_stripes(write_data_context* wtc) {
    LIST_ENTRY *le, *le2, *nextle;
    
    le = wtc->stripes.Flink;
    while (le != &wtc->stripes) {
        write_data_stripe* stripe = CONTAINING_RECORD(le, write_data_stripe, list_entry);
        
        if (stripe->Irp) {
            if (stripe->device->devobj->Flags & DO_DIRECT_IO) {
                MmUnlockPages(stripe->Irp->MdlAddress);
                IoFreeMdl(stripe->Irp->MdlAddress);
            }
        }
        
        le = le->Flink;
    }
    
    le = wtc->stripes.Flink;
    while (le != &wtc->stripes) {
        write_data_stripe* stripe = CONTAINING_RECORD(le, write_data_stripe, list_entry);
        
        nextle = le->Flink;

        if (stripe->buf && stripe->need_free) {
            ExFreePool(stripe->buf);
            
            le2 = le->Flink;
            while (le2 != &wtc->stripes) {
                write_data_stripe* s2 = CONTAINING_RECORD(le2, write_data_stripe, list_entry);
                
                if (s2->buf == stripe->buf)
                    s2->buf = NULL;
                
                le2 = le2->Flink;
            }
            
        }
        
        ExFreePool(stripe);
        
        le = nextle;
    }
}

static changed_extent* get_changed_extent_item(chunk* c, UINT64 address, UINT64 size, BOOL no_csum) {
    LIST_ENTRY* le;
    changed_extent* ce;
    
    le = c->changed_extents.Flink;
    while (le != &c->changed_extents) {
        ce = CONTAINING_RECORD(le, changed_extent, list_entry);
        
        if (ce->address == address && ce->size == size)
            return ce;
        
        le = le->Flink;
    }
    
    ce = ExAllocatePoolWithTag(PagedPool, sizeof(changed_extent), ALLOC_TAG);
    if (!ce) {
        ERR("out of memory\n");
        return NULL;
    }
    
    ce->address = address;
    ce->size = size;
    ce->old_size = size;
    ce->count = 0;
    ce->old_count = 0;
    ce->no_csum = no_csum;
    InitializeListHead(&ce->refs);
    InitializeListHead(&ce->old_refs);
    
    InsertTailList(&c->changed_extents, &ce->list_entry);
    
    return ce;
}

NTSTATUS update_changed_extent_ref(device_extension* Vcb, chunk* c, UINT64 address, UINT64 size, UINT64 root, UINT64 objid, UINT64 offset, signed long long count,
                                   BOOL no_csum, UINT64 new_size, PIRP Irp) {
    LIST_ENTRY* le;
    changed_extent* ce;
    changed_extent_ref* cer;
    NTSTATUS Status;
    KEY searchkey;
    traverse_ptr tp;
    UINT64 old_count;
    
    ExAcquireResourceExclusiveLite(&c->changed_extents_lock, TRUE);
    
    ce = get_changed_extent_item(c, address, size, no_csum);
    
    if (!ce) {
        ERR("get_changed_extent_item failed\n");
        Status = STATUS_INTERNAL_ERROR;
        goto end;
    }
    
    if (IsListEmpty(&ce->refs) && IsListEmpty(&ce->old_refs)) { // new entry
        searchkey.obj_id = address;
        searchkey.obj_type = TYPE_EXTENT_ITEM;
        searchkey.offset = 0xffffffffffffffff;
        
        Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, FALSE, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("error - find_item returned %08x\n", Status);
            goto end;
        }
        
        if (tp.item->key.obj_id != searchkey.obj_id || tp.item->key.obj_type != searchkey.obj_type) {
            ERR("could not find address %llx in extent tree\n", address);
            Status = STATUS_INTERNAL_ERROR;
            goto end;
        }
        
        if (tp.item->key.offset != size) {
            ERR("extent %llx had size %llx, not %llx as expected\n", address, tp.item->key.offset, size);
            Status = STATUS_INTERNAL_ERROR;
            goto end;
        }
        
        if (tp.item->size == sizeof(EXTENT_ITEM_V0)) {
            EXTENT_ITEM_V0* eiv0 = (EXTENT_ITEM_V0*)tp.item->data;
            
            ce->count = ce->old_count = eiv0->refcount;
        } else if (tp.item->size >= sizeof(EXTENT_ITEM)) {
            EXTENT_ITEM* ei = (EXTENT_ITEM*)tp.item->data;
            
            ce->count = ce->old_count = ei->refcount;
        } else {
            ERR("(%llx,%x,%llx) was %u bytes, expected at least %u\n", tp.item->key.obj_id, tp.item->key.obj_type, tp.item->key.offset, tp.item->size, sizeof(EXTENT_ITEM));
            Status = STATUS_INTERNAL_ERROR;
            goto end;
        }
    }
    
    ce->size = new_size;
    
    le = ce->refs.Flink;
    while (le != &ce->refs) {
        cer = CONTAINING_RECORD(le, changed_extent_ref, list_entry);
        
        if (cer->edr.root == root && cer->edr.objid == objid && cer->edr.offset == offset) {
            ce->count += count;
            cer->edr.count += count;
            Status = STATUS_SUCCESS;
            goto end;
        }
        
        le = le->Flink;
    }
    
    old_count = find_extent_data_refcount(Vcb, address, size, root, objid, offset, Irp);
    
    if (old_count > 0) {
        cer = ExAllocatePoolWithTag(PagedPool, sizeof(changed_extent_ref), ALLOC_TAG);
    
        if (!cer) {
            ERR("out of memory\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }
        
        cer->edr.root = root;
        cer->edr.objid = objid;
        cer->edr.offset = offset;
        cer->edr.count = old_count;
        
        InsertTailList(&ce->old_refs, &cer->list_entry);
    }
    
    cer = ExAllocatePoolWithTag(PagedPool, sizeof(changed_extent_ref), ALLOC_TAG);
    
    if (!cer) {
        ERR("out of memory\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }
    
    cer->edr.root = root;
    cer->edr.objid = objid;
    cer->edr.offset = offset;
    cer->edr.count = old_count + count;
    
    InsertTailList(&ce->refs, &cer->list_entry);
    
    ce->count += count;
    
    Status = STATUS_SUCCESS;
    
end:
    ExReleaseResourceLite(&c->changed_extents_lock);
    
    return Status;
}

NTSTATUS excise_extents(device_extension* Vcb, fcb* fcb, UINT64 start_data, UINT64 end_data, PIRP Irp, LIST_ENTRY* rollback) {
    NTSTATUS Status;
    LIST_ENTRY* le;
    
    le = fcb->extents.Flink;

    while (le != &fcb->extents) {
        LIST_ENTRY* le2 = le->Flink;
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);
        EXTENT_DATA* ed = ext->data;
        EXTENT_DATA2* ed2;
        UINT64 len;
        
        if (!ext->ignore) {
            if (ext->datalen < sizeof(EXTENT_DATA)) {
                ERR("extent at %llx was %u bytes, expected at least %u\n", ext->offset, ext->datalen, sizeof(EXTENT_DATA));
                Status = STATUS_INTERNAL_ERROR;
                goto end;
            }
            
            if (ed->type == EXTENT_TYPE_REGULAR || ed->type == EXTENT_TYPE_PREALLOC) { 
                if (ext->datalen < sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2)) {
                    ERR("extent at %llx was %u bytes, expected at least %u\n", ext->offset, ext->datalen, sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2));
                    Status = STATUS_INTERNAL_ERROR;
                    goto end;
                }
                
                ed2 = (EXTENT_DATA2*)ed->data;
            }
            
            len = ed->type == EXTENT_TYPE_INLINE ? ed->decoded_size : ed2->num_bytes;
            
            if (ext->offset < end_data && ext->offset + len > start_data) {
                if (ed->type == EXTENT_TYPE_INLINE) {
                    if (start_data <= ext->offset && end_data >= ext->offset + len) { // remove all
                        remove_fcb_extent(fcb, ext, rollback);
                        
                        fcb->inode_item.st_blocks -= len;
                    } else if (start_data <= ext->offset && end_data < ext->offset + len) { // remove beginning
                        EXTENT_DATA* ned;
                        UINT64 size;
                        extent* newext;
                        
                        size = len - (end_data - ext->offset);
                        
                        ned = ExAllocatePoolWithTag(PagedPool, sizeof(EXTENT_DATA) - 1 + size, ALLOC_TAG);
                        if (!ned) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            goto end;
                        }
                        
                        newext = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
                        if (!newext) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            ExFreePool(ned);
                            goto end;
                        }
                        
                        ned->generation = Vcb->superblock.generation;
                        ned->decoded_size = size;
                        ned->compression = ed->compression;
                        ned->encryption = ed->encryption;
                        ned->encoding = ed->encoding;
                        ned->type = ed->type;
                        
                        RtlCopyMemory(&ned->data[0], &ed->data[end_data - ext->offset], size);
                        
                        newext->offset = end_data;
                        newext->data = ned;
                        newext->datalen = sizeof(EXTENT_DATA) - 1 + size;
                        newext->unique = ext->unique;
                        newext->ignore = FALSE;
                        InsertHeadList(&ext->list_entry, &newext->list_entry);
                        
                        remove_fcb_extent(fcb, ext, rollback);
                        
                        fcb->inode_item.st_blocks -= end_data - ext->offset;
                    } else if (start_data > ext->offset && end_data >= ext->offset + len) { // remove end
                        EXTENT_DATA* ned;
                        UINT64 size;
                        extent* newext;
                        
                        size = start_data - ext->offset;
                        
                        ned = ExAllocatePoolWithTag(PagedPool, sizeof(EXTENT_DATA) - 1 + size, ALLOC_TAG);
                        if (!ned) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            goto end;
                        }
                        
                        newext = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
                        if (!newext) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            ExFreePool(ned);
                            goto end;
                        }
                        
                        ned->generation = Vcb->superblock.generation;
                        ned->decoded_size = size;
                        ned->compression = ed->compression;
                        ned->encryption = ed->encryption;
                        ned->encoding = ed->encoding;
                        ned->type = ed->type;
                        
                        RtlCopyMemory(&ned->data[0], &ed->data[0], size);
                        
                        newext->offset = ext->offset;
                        newext->data = ned;
                        newext->datalen = sizeof(EXTENT_DATA) - 1 + size;
                        newext->unique = ext->unique;
                        newext->ignore = FALSE;
                        InsertHeadList(&ext->list_entry, &newext->list_entry);
                        
                        remove_fcb_extent(fcb, ext, rollback);
                        
                        fcb->inode_item.st_blocks -= ext->offset + len - start_data;
                    } else if (start_data > ext->offset && end_data < ext->offset + len) { // remove middle
                        EXTENT_DATA *ned1, *ned2;
                        UINT64 size;
                        extent *newext1, *newext2;
                        
                        size = start_data - ext->offset;
                        
                        ned1 = ExAllocatePoolWithTag(PagedPool, sizeof(EXTENT_DATA) - 1 + size, ALLOC_TAG);
                        if (!ned1) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            goto end;
                        }
                        
                        newext1 = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
                        if (!newext1) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            ExFreePool(ned1);
                            goto end;
                        }
                        
                        ned1->generation = Vcb->superblock.generation;
                        ned1->decoded_size = size;
                        ned1->compression = ed->compression;
                        ned1->encryption = ed->encryption;
                        ned1->encoding = ed->encoding;
                        ned1->type = ed->type;
                        
                        RtlCopyMemory(&ned1->data[0], &ed->data[0], size);

                        newext1->offset = ext->offset;
                        newext1->data = ned1;
                        newext1->datalen = sizeof(EXTENT_DATA) - 1 + size;
                        newext1->unique = FALSE;
                        newext1->ignore = FALSE;
                        
                        size = ext->offset + len - end_data;
                        
                        ned2 = ExAllocatePoolWithTag(PagedPool, sizeof(EXTENT_DATA) - 1 + size, ALLOC_TAG);
                        if (!ned2) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            ExFreePool(ned1);
                            ExFreePool(newext1);
                            goto end;
                        }
                        
                        newext2 = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
                        if (!newext2) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            ExFreePool(ned1);
                            ExFreePool(newext1);
                            ExFreePool(ned2);
                            goto end;
                        }
                        
                        ned2->generation = Vcb->superblock.generation;
                        ned2->decoded_size = size;
                        ned2->compression = ed->compression;
                        ned2->encryption = ed->encryption;
                        ned2->encoding = ed->encoding;
                        ned2->type = ed->type;
                        
                        RtlCopyMemory(&ned2->data[0], &ed->data[end_data - ext->offset], size);
                        
                        newext2->offset = end_data;
                        newext2->data = ned2;
                        newext2->datalen = sizeof(EXTENT_DATA) - 1 + size;
                        newext2->unique = FALSE;
                        newext2->ignore = FALSE;
                        
                        InsertHeadList(&ext->list_entry, &newext1->list_entry);
                        InsertHeadList(&newext1->list_entry, &newext2->list_entry);
                        
                        remove_fcb_extent(fcb, ext, rollback);
                        
                        fcb->inode_item.st_blocks -= end_data - start_data;
                    }
                } else if (ed->type == EXTENT_TYPE_REGULAR || ed->type == EXTENT_TYPE_PREALLOC) {
                    if (start_data <= ext->offset && end_data >= ext->offset + len) { // remove all
                        if (ed2->address != 0) {
                            chunk* c;
                            
                            fcb->inode_item.st_blocks -= len;
                            
                            c = get_chunk_from_address(Vcb, ed2->address);
                            
                            if (!c) {
                                ERR("get_chunk_from_address(%llx) failed\n", ed2->address);
                            } else {
                                Status = update_changed_extent_ref(Vcb, c, ed2->address, ed2->size, fcb->subvol->id, fcb->inode, ext->offset - ed2->offset, -1,
                                                                   fcb->inode_item.flags & BTRFS_INODE_NODATASUM, ed2->size, Irp);
                                if (!NT_SUCCESS(Status)) {
                                    ERR("update_changed_extent_ref returned %08x\n", Status);
                                    goto end;
                                }
                            }
                        }
                        
                        remove_fcb_extent(fcb, ext, rollback);
                    } else if (start_data <= ext->offset && end_data < ext->offset + len) { // remove beginning
                        EXTENT_DATA* ned;
                        EXTENT_DATA2* ned2;
                        extent* newext;
                        
                        if (ed2->address != 0)
                            fcb->inode_item.st_blocks -= end_data - ext->offset;
                        
                        ned = ExAllocatePoolWithTag(PagedPool, sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2), ALLOC_TAG);
                        if (!ned) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            goto end;
                        }
                        
                        newext = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
                        if (!newext) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            ExFreePool(ned);
                            goto end;
                        }
                        
                        ned2 = (EXTENT_DATA2*)&ned->data[0];
                        
                        ned->generation = Vcb->superblock.generation;
                        ned->decoded_size = ed->decoded_size;
                        ned->compression = ed->compression;
                        ned->encryption = ed->encryption;
                        ned->encoding = ed->encoding;
                        ned->type = ed->type;
                        ned2->address = ed2->address;
                        ned2->size = ed2->size;
                        ned2->offset = ed2->address == 0 ? 0 : (ed2->offset + (end_data - ext->offset));
                        ned2->num_bytes = ed2->num_bytes - (end_data - ext->offset);

                        newext->offset = end_data;
                        newext->data = ned;
                        newext->datalen = sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2);
                        newext->unique = ext->unique;
                        newext->ignore = FALSE;
                        InsertHeadList(&ext->list_entry, &newext->list_entry);
                        
                        remove_fcb_extent(fcb, ext, rollback);
                    } else if (start_data > ext->offset && end_data >= ext->offset + len) { // remove end
                        EXTENT_DATA* ned;
                        EXTENT_DATA2* ned2;
                        extent* newext;
                        
                        if (ed2->address != 0)
                            fcb->inode_item.st_blocks -= ext->offset + len - start_data;
                        
                        ned = ExAllocatePoolWithTag(PagedPool, sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2), ALLOC_TAG);
                        if (!ned) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            goto end;
                        }
                        
                        newext = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
                        if (!newext) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            ExFreePool(ned);
                            goto end;
                        }
                        
                        ned2 = (EXTENT_DATA2*)&ned->data[0];
                        
                        ned->generation = Vcb->superblock.generation;
                        ned->decoded_size = ed->decoded_size;
                        ned->compression = ed->compression;
                        ned->encryption = ed->encryption;
                        ned->encoding = ed->encoding;
                        ned->type = ed->type;
                        ned2->address = ed2->address;
                        ned2->size = ed2->size;
                        ned2->offset = ed2->address == 0 ? 0 : ed2->offset;
                        ned2->num_bytes = start_data - ext->offset;

                        newext->offset = ext->offset;
                        newext->data = ned;
                        newext->datalen = sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2);
                        newext->unique = ext->unique;
                        newext->ignore = FALSE;
                        InsertHeadList(&ext->list_entry, &newext->list_entry);
                        
                        remove_fcb_extent(fcb, ext, rollback);
                    } else if (start_data > ext->offset && end_data < ext->offset + len) { // remove middle
                        EXTENT_DATA *neda, *nedb;
                        EXTENT_DATA2 *neda2, *nedb2;
                        extent *newext1, *newext2;
                        
                        if (ed2->address != 0) {
                            chunk* c;
                            
                            fcb->inode_item.st_blocks -= end_data - start_data;
                            
                            c = get_chunk_from_address(Vcb, ed2->address);
                            
                            if (!c) {
                                ERR("get_chunk_from_address(%llx) failed\n", ed2->address);
                            } else {
                                Status = update_changed_extent_ref(Vcb, c, ed2->address, ed2->size, fcb->subvol->id, fcb->inode, ext->offset - ed2->offset, 1,
                                                                   fcb->inode_item.flags & BTRFS_INODE_NODATASUM, ed2->size, Irp);
                                if (!NT_SUCCESS(Status)) {
                                    ERR("update_changed_extent_ref returned %08x\n", Status);
                                    goto end;
                                }
                            }
                        }
                        
                        neda = ExAllocatePoolWithTag(PagedPool, sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2), ALLOC_TAG);
                        if (!neda) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            goto end;
                        }
                        
                        newext1 = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
                        if (!newext1) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            ExFreePool(neda);
                            goto end;
                        }
                        
                        nedb = ExAllocatePoolWithTag(PagedPool, sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2), ALLOC_TAG);
                        if (!nedb) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            ExFreePool(neda);
                            ExFreePool(newext1);
                            goto end;
                        }
                        
                        newext2 = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
                        if (!newext1) {
                            ERR("out of memory\n");
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            ExFreePool(neda);
                            ExFreePool(newext1);
                            ExFreePool(nedb);
                            goto end;
                        }
                        
                        neda2 = (EXTENT_DATA2*)&neda->data[0];
                        
                        neda->generation = Vcb->superblock.generation;
                        neda->decoded_size = ed->decoded_size;
                        neda->compression = ed->compression;
                        neda->encryption = ed->encryption;
                        neda->encoding = ed->encoding;
                        neda->type = ed->type;
                        neda2->address = ed2->address;
                        neda2->size = ed2->size;
                        neda2->offset = ed2->address == 0 ? 0 : ed2->offset;
                        neda2->num_bytes = start_data - ext->offset;

                        nedb2 = (EXTENT_DATA2*)&nedb->data[0];
                        
                        nedb->generation = Vcb->superblock.generation;
                        nedb->decoded_size = ed->decoded_size;
                        nedb->compression = ed->compression;
                        nedb->encryption = ed->encryption;
                        nedb->encoding = ed->encoding;
                        nedb->type = ed->type;
                        nedb2->address = ed2->address;
                        nedb2->size = ed2->size;
                        nedb2->offset = ed2->address == 0 ? 0 : (ed2->offset + (end_data - ext->offset));
                        nedb2->num_bytes = ext->offset + len - end_data;
                        
                        newext1->offset = ext->offset;
                        newext1->data = neda;
                        newext1->datalen = sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2);
                        newext1->unique = FALSE;
                        newext1->ignore = FALSE;
                        
                        newext2->offset = end_data;
                        newext2->data = nedb;
                        newext2->datalen = sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2);
                        newext2->unique = FALSE;
                        newext2->ignore = FALSE;
                        
                        InsertHeadList(&ext->list_entry, &newext1->list_entry);
                        InsertHeadList(&newext1->list_entry, &newext2->list_entry);
                        
                        remove_fcb_extent(fcb, ext, rollback);
                    }
                }
            }
        }

        le = le2;
    }
    
    // FIXME - do bitmap analysis of changed extents, and free what we can
    
    Status = STATUS_SUCCESS;

end:
    fcb->extents_changed = TRUE;
    mark_fcb_dirty(fcb);
    
    return Status;
}

static NTSTATUS do_write_data(device_extension* Vcb, UINT64 address, void* data, UINT64 length, LIST_ENTRY* changed_sector_list, PIRP Irp) {
    NTSTATUS Status;
    changed_sector* sc;
    int i;
    
    Status = write_data_complete(Vcb, address, data, length, Irp, NULL);
    if (!NT_SUCCESS(Status)) {
        ERR("write_data returned %08x\n", Status);
        return Status;
    }
    
    if (changed_sector_list) {
        sc = ExAllocatePoolWithTag(PagedPool, sizeof(changed_sector), ALLOC_TAG);
        if (!sc) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        sc->ol.key = address;
        sc->length = length / Vcb->superblock.sector_size;
        sc->deleted = FALSE;
        
        sc->checksums = ExAllocatePoolWithTag(PagedPool, sizeof(UINT32) * sc->length, ALLOC_TAG);
        if (!sc->checksums) {
            ERR("out of memory\n");
            ExFreePool(sc);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        for (i = 0; i < sc->length; i++) {
            sc->checksums[i] = ~calc_crc32c(0xffffffff, (UINT8*)data + (i * Vcb->superblock.sector_size), Vcb->superblock.sector_size);
        }

        insert_into_ordered_list(changed_sector_list, &sc->ol);
    }
    
    return STATUS_SUCCESS;
}

static void add_insert_extent_rollback(LIST_ENTRY* rollback, fcb* fcb, extent* ext) {
    rollback_extent* re;
    
    re = ExAllocatePoolWithTag(NonPagedPool, sizeof(rollback_extent), ALLOC_TAG);
    if (!re) {
        ERR("out of memory\n");
        return;
    }
    
    re->fcb = fcb;
    re->ext = ext;
    
    add_rollback(rollback, ROLLBACK_INSERT_EXTENT, re);
}

static BOOL add_extent_to_fcb(fcb* fcb, UINT64 offset, EXTENT_DATA* ed, ULONG edsize, BOOL unique, LIST_ENTRY* rollback) {
    extent* ext;
    LIST_ENTRY* le;
    
    ext = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
    if (!ext) {
        ERR("out of memory\n");
        return FALSE;
    }
    
    ext->offset = offset;
    ext->data = ed;
    ext->datalen = edsize;
    ext->unique = unique;
    ext->ignore = FALSE;
    
    le = fcb->extents.Flink;
    while (le != &fcb->extents) {
        extent* oldext = CONTAINING_RECORD(le, extent, list_entry);
        
        if (!oldext->ignore) {
            if (oldext->offset > offset) {
                InsertHeadList(le->Blink, &ext->list_entry);
                goto end;
            }
        }
        
        le = le->Flink;
    }
    
    InsertTailList(&fcb->extents, &ext->list_entry);
    
end:
    add_insert_extent_rollback(rollback, fcb, ext);

    return TRUE;
}

static void remove_fcb_extent(fcb* fcb, extent* ext, LIST_ENTRY* rollback) {
    if (!ext->ignore) {
        rollback_extent* re;
        
        ext->ignore = TRUE;
        
        re = ExAllocatePoolWithTag(NonPagedPool, sizeof(rollback_extent), ALLOC_TAG);
        if (!re) {
            ERR("out of memory\n");
            return;
        }
        
        re->fcb = fcb;
        re->ext = ext;
        
        add_rollback(rollback, ROLLBACK_DELETE_EXTENT, re);
    }
}

static void add_changed_extent_ref(chunk* c, UINT64 address, UINT64 size, UINT64 root, UINT64 objid, UINT64 offset, UINT32 count, BOOL no_csum) {
    changed_extent* ce;
    changed_extent_ref* cer;
    LIST_ENTRY* le;
    
    ce = get_changed_extent_item(c, address, size, no_csum);
    
    if (!ce) {
        ERR("get_changed_extent_item failed\n");
        return;
    }
    
    le = ce->refs.Flink;
    while (le != &ce->refs) {
        cer = CONTAINING_RECORD(le, changed_extent_ref, list_entry);
        
        if (cer->edr.root == root && cer->edr.objid == objid && cer->edr.offset == offset) {
            ce->count += count;
            cer->edr.count += count;
            return;
        }
        
        le = le->Flink;
    }
    
    cer = ExAllocatePoolWithTag(PagedPool, sizeof(changed_extent_ref), ALLOC_TAG);
    
    if (!cer) {
        ERR("out of memory\n");
        return;
    }
    
    cer->edr.root = root;
    cer->edr.objid = objid;
    cer->edr.offset = offset;
    cer->edr.count = count;
    
    InsertTailList(&ce->refs, &cer->list_entry);
    
    ce->count += count;
}

BOOL insert_extent_chunk(device_extension* Vcb, fcb* fcb, chunk* c, UINT64 start_data, UINT64 length, BOOL prealloc, void* data,
                         LIST_ENTRY* changed_sector_list, PIRP Irp, LIST_ENTRY* rollback, UINT8 compression, UINT64 decoded_size) {
    UINT64 address;
    NTSTATUS Status;
    EXTENT_DATA* ed;
    EXTENT_DATA2* ed2;
    ULONG edsize = sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2);
// #ifdef DEBUG_PARANOID
//     traverse_ptr tp;
//     KEY searchkey;
// #endif
    
    TRACE("(%p, (%llx, %llx), %llx, %llx, %llx, %u, %p, %p, %p)\n", Vcb, fcb->subvol->id, fcb->inode, c->offset, start_data, length, prealloc, data, changed_sector_list, rollback);
    
    if (!find_address_in_chunk(Vcb, c, length, &address))
        return FALSE;
    
// #ifdef DEBUG_PARANOID
//     searchkey.obj_id = address;
//     searchkey.obj_type = TYPE_EXTENT_ITEM;
//     searchkey.offset = 0xffffffffffffffff;
//     
//     Status = find_item(Vcb, Vcb->extent_root, &tp, &searchkey, FALSE);
//     if (!NT_SUCCESS(Status)) {
//         ERR("error - find_item returned %08x\n", Status);
//     } else if (tp.item->key.obj_id == searchkey.obj_id && tp.item->key.obj_type == searchkey.obj_type) {
//         ERR("address %llx already allocated\n", address);
//         int3;
//     }
// #endif
    
    if (data) {
        Status = do_write_data(Vcb, address, data, length, changed_sector_list, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("do_write_data returned %08x\n", Status);
            return FALSE;
        }
    }
    
    // add extent data to inode
    ed = ExAllocatePoolWithTag(PagedPool, edsize, ALLOC_TAG);
    if (!ed) {
        ERR("out of memory\n");
        return FALSE;
    }
    
    ed->generation = Vcb->superblock.generation;
    ed->decoded_size = decoded_size;
    ed->compression = compression;
    ed->encryption = BTRFS_ENCRYPTION_NONE;
    ed->encoding = BTRFS_ENCODING_NONE;
    ed->type = prealloc ? EXTENT_TYPE_PREALLOC : EXTENT_TYPE_REGULAR;
    
    ed2 = (EXTENT_DATA2*)ed->data;
    ed2->address = address;
    ed2->size = length;
    ed2->offset = 0;
    ed2->num_bytes = decoded_size;
    
    if (!add_extent_to_fcb(fcb, start_data, ed, edsize, TRUE, rollback)) {
        ERR("add_extent_to_fcb failed\n");
        ExFreePool(ed);
        return FALSE;
    }
    
    increase_chunk_usage(c, length);
    space_list_subtract(Vcb, c, FALSE, address, length, rollback);
    
    fcb->inode_item.st_blocks += decoded_size;
    
    fcb->extents_changed = TRUE;
    mark_fcb_dirty(fcb);
    
    ExAcquireResourceExclusiveLite(&c->changed_extents_lock, TRUE);
    
    add_changed_extent_ref(c, address, length, fcb->subvol->id, fcb->inode, start_data, 1, fcb->inode_item.flags & BTRFS_INODE_NODATASUM);
    
    ExReleaseResourceLite(&c->changed_extents_lock);

    return TRUE;
}

static BOOL extend_data(device_extension* Vcb, fcb* fcb, UINT64 start_data, UINT64 length, void* data,
                        LIST_ENTRY* changed_sector_list, extent* ext, chunk* c, PIRP Irp, LIST_ENTRY* rollback) {
    EXTENT_DATA* ed;
    EXTENT_DATA2 *ed2, *ed2orig;
    extent* newext;
    UINT64 addr, origsize;
    NTSTATUS Status;
    LIST_ENTRY* le;
    
    TRACE("(%p, (%llx, %llx), %llx, %llx, %p, %p, %p, %p)\n", Vcb, fcb->subvol->id, fcb->inode, start_data,
                                                              length, data, changed_sector_list, ext, c, rollback);
    
    ed2orig = (EXTENT_DATA2*)ext->data->data;
    
    origsize = ed2orig->size;
    addr = ed2orig->address + ed2orig->size;
    
    Status = write_data_complete(Vcb, addr, data, length, Irp, c);
    if (!NT_SUCCESS(Status)) {
        ERR("write_data returned %08x\n", Status);
        return FALSE;
    }
    
    le = fcb->extents.Flink;
    while (le != &fcb->extents) {
        extent* ext2 = CONTAINING_RECORD(le, extent, list_entry);
            
        if (!ext2->ignore && (ext2->data->type == EXTENT_TYPE_REGULAR || ext2->data->type == EXTENT_TYPE_PREALLOC)) {
            EXTENT_DATA2* ed2b = (EXTENT_DATA2*)ext2->data->data;
            
            if (ed2b->address == ed2orig->address) {
                ed2b->size = origsize + length;
                ext2->data->decoded_size = origsize + length;
            }
        }
                
        le = le->Flink;
    }
    
    ed = ExAllocatePoolWithTag(PagedPool, ext->datalen, ALLOC_TAG);
    if (!ed) {
        ERR("out of memory\n");
        return FALSE;
    }
    
    newext = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
    if (!newext) {
        ERR("out of memory\n");
        ExFreePool(ed);
        return FALSE;
    }
    
    RtlCopyMemory(ed, ext->data, ext->datalen);

    ed2 = (EXTENT_DATA2*)ed->data;
    ed2->offset = ed2orig->offset + ed2orig->num_bytes;
    ed2->num_bytes = length;
    
    RtlCopyMemory(newext, ext, sizeof(extent));
    newext->offset = ext->offset + ed2orig->num_bytes;
    newext->data = ed;
    
    InsertHeadList(&ext->list_entry, &newext->list_entry);
    
    add_insert_extent_rollback(rollback, fcb, newext);
    
    Status = update_changed_extent_ref(Vcb, c, ed2orig->address, origsize, fcb->subvol->id, fcb->inode, newext->offset - ed2->offset,
                                       1, fcb->inode_item.flags & BTRFS_INODE_NODATASUM, ed2->size, Irp);

    if (!NT_SUCCESS(Status)) {
        ERR("update_changed_extent_ref returned %08x\n", Status);
        return FALSE;
    }
    
    if (changed_sector_list) {
        int i;
        changed_sector* sc = ExAllocatePoolWithTag(PagedPool, sizeof(changed_sector), ALLOC_TAG);
        if (!sc) {
            ERR("out of memory\n");
            return FALSE;
        }
        
        sc->ol.key = addr;
        sc->length = length / Vcb->superblock.sector_size;
        sc->deleted = FALSE;
        
        sc->checksums = ExAllocatePoolWithTag(PagedPool, sizeof(UINT32) * sc->length, ALLOC_TAG);
        if (!sc->checksums) {
            ERR("out of memory\n");
            ExFreePool(sc);
            return FALSE;
        }
        
        for (i = 0; i < sc->length; i++) {
            sc->checksums[i] = ~calc_crc32c(0xffffffff, (UINT8*)data + (i * Vcb->superblock.sector_size), Vcb->superblock.sector_size);
        }
        insert_into_ordered_list(changed_sector_list, &sc->ol);
    }
    
    increase_chunk_usage(c, length);
      
    space_list_subtract(Vcb, c, FALSE, addr, length, NULL); // no rollback as we don't reverse extending the extent
     
    fcb->inode_item.st_blocks += length;
    
    return TRUE;
}

static BOOL try_extend_data(device_extension* Vcb, fcb* fcb, UINT64 start_data, UINT64 length, void* data,
                            LIST_ENTRY* changed_sector_list, PIRP Irp, UINT64* written, LIST_ENTRY* rollback) {
    BOOL success = FALSE;
    EXTENT_DATA* ed;
    EXTENT_DATA2* ed2;
    chunk* c;
    LIST_ENTRY* le;
    space* s;
    extent* ext = NULL;
    
    le = fcb->extents.Flink;
    
    while (le != &fcb->extents) {
        extent* nextext = CONTAINING_RECORD(le, extent, list_entry);
        
        if (!nextext->ignore) {
            if (nextext->offset == start_data) {
                ext = nextext;
                break;
            } else if (nextext->offset > start_data)
                break;
            
            ext = nextext;
        }
        
        le = le->Flink;
    }
    
    if (!ext)
        return FALSE;

    if (!ext->unique) {
        TRACE("extent was not unique\n");
        return FALSE;
    }
    
    ed = ext->data;
    
    if (ext->datalen < sizeof(EXTENT_DATA)) {
        ERR("extent %llx was %u bytes, expected at least %u\n", ext->offset, ext->datalen, sizeof(EXTENT_DATA));
        return FALSE;
    }
    
    if (ed->type != EXTENT_TYPE_REGULAR) {
        TRACE("not extending extent which is not EXTENT_TYPE_REGULAR\n");
        return FALSE;
    }
    
    ed2 = (EXTENT_DATA2*)ed->data;
    
    if (ext->datalen < sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2)) {
        ERR("extent %llx was %u bytes, expected at least %u\n", ext->offset, ext->datalen, sizeof(EXTENT_DATA) - 1 + sizeof(EXTENT_DATA2));
        return FALSE;
    }

    if (ext->offset + ed2->num_bytes != start_data) {
        TRACE("last EXTENT_DATA does not run up to start_data (%llx + %llx != %llx)\n", ext->offset, ed2->num_bytes, start_data);
        return FALSE;
    }
    
    if (ed->compression != BTRFS_COMPRESSION_NONE) {
        TRACE("not extending a compressed extent\n");
        return FALSE;
    }
    
    if (ed->encryption != BTRFS_ENCRYPTION_NONE) {
        WARN("encryption not supported\n");
        return FALSE;
    }
    
    if (ed->encoding != BTRFS_ENCODING_NONE) {
        WARN("other encodings not supported\n");
        return FALSE;
    }
    
    if (ed2->size - ed2->offset != ed2->num_bytes) {
        TRACE("last EXTENT_DATA does not run all the way to the end of the extent\n");
        return FALSE;
    }
    
    if (ed2->size >= MAX_EXTENT_SIZE) {
        TRACE("extent size was too large to extend (%llx >= %llx)\n", ed2->size, (UINT64)MAX_EXTENT_SIZE);
        return FALSE;
    }
    
    c = get_chunk_from_address(Vcb, ed2->address);
    
    ExAcquireResourceExclusiveLite(&c->lock, TRUE);
    
    le = c->space.Flink;
    while (le != &c->space) {
        s = CONTAINING_RECORD(le, space, list_entry);
        
        if (s->address == ed2->address + ed2->size) {
            UINT64 newlen = min(min(s->size, length), MAX_EXTENT_SIZE - ed2->size);
            
            success = extend_data(Vcb, fcb, start_data, newlen, data, changed_sector_list, ext, c, Irp, rollback);
            
            if (success)
                *written += newlen;
            
            break;
        } else if (s->address > ed2->address + ed2->size)
            break;
        
        le = le->Flink;
    }
    
    ExReleaseResourceLite(&c->lock);
    
    return success;
}

static NTSTATUS insert_prealloc_extent(fcb* fcb, UINT64 start, UINT64 length, LIST_ENTRY* rollback) {
    LIST_ENTRY* le;
    chunk* c;
    UINT64 flags, origlength = length;
    NTSTATUS Status;
    BOOL page_file = fcb->Header.Flags2 & FSRTL_FLAG2_IS_PAGING_FILE;
    
    flags = fcb->Vcb->data_flags;
    
    // FIXME - try and maximize contiguous ranges first. If we can't do that,
    // allocate all the free space we find until it's enough.
    
    do {
        UINT64 extlen = min(MAX_EXTENT_SIZE, length);
        
        ExAcquireResourceSharedLite(&fcb->Vcb->chunk_lock, TRUE);
        
        le = fcb->Vcb->chunks.Flink;
        while (le != &fcb->Vcb->chunks) {
            c = CONTAINING_RECORD(le, chunk, list_entry);
            
            ExAcquireResourceExclusiveLite(&c->lock, TRUE);
            
            if (c->chunk_item->type == flags && (c->chunk_item->size - c->used) >= extlen) {
                if (insert_extent_chunk(fcb->Vcb, fcb, c, start, extlen, !page_file, NULL, NULL, NULL, rollback, BTRFS_COMPRESSION_NONE, extlen)) {
                    ExReleaseResourceLite(&c->lock);
                    ExReleaseResourceLite(&fcb->Vcb->chunk_lock);
                    goto cont;
                }
            }
            
            ExReleaseResourceLite(&c->lock);

            le = le->Flink;
        }
        
        ExReleaseResourceLite(&fcb->Vcb->chunk_lock);
        
        ExAcquireResourceExclusiveLite(&fcb->Vcb->chunk_lock, TRUE);
        
        if ((c = alloc_chunk(fcb->Vcb, flags))) {
            ExReleaseResourceLite(&fcb->Vcb->chunk_lock);
            
            ExAcquireResourceExclusiveLite(&c->lock, TRUE);
            
            if (c->chunk_item->type == flags && (c->chunk_item->size - c->used) >= extlen) {
                if (insert_extent_chunk(fcb->Vcb, fcb, c, start, extlen, !page_file, NULL, NULL, NULL, rollback, BTRFS_COMPRESSION_NONE, extlen)) {
                    ExReleaseResourceLite(&c->lock);
                    goto cont;
                }
            }
            
            ExReleaseResourceLite(&c->lock);
        } else
            ExReleaseResourceLite(&fcb->Vcb->chunk_lock);
        
        WARN("couldn't find any data chunks with %llx bytes free\n", origlength);
        Status = STATUS_DISK_FULL;
        goto end;
        
cont:
        length -= extlen;
        start += extlen;
    } while (length > 0);
    
    Status = STATUS_SUCCESS;
    
end:
    return Status;
}

// static void print_tree(tree* t) {
//     LIST_ENTRY* le = t->itemlist.Flink;
//     while (le != &t->itemlist) {
//         tree_data* td = CONTAINING_RECORD(le, tree_data, list_entry);
//         ERR("%llx,%x,%llx (ignore = %s)\n", td->key.obj_id, td->key.obj_type, td->key.offset, td->ignore ? "TRUE" : "FALSE");
//         le = le->Flink;
//     }
// }

NTSTATUS insert_extent(device_extension* Vcb, fcb* fcb, UINT64 start_data, UINT64 length, void* data, LIST_ENTRY* changed_sector_list, PIRP Irp, LIST_ENTRY* rollback) {
    LIST_ENTRY* le;
    chunk* c;
    UINT64 flags, orig_length = length, written = 0;
    
    TRACE("(%p, (%llx, %llx), %llx, %llx, %p, %p)\n", Vcb, fcb->subvol->id, fcb->inode, start_data, length, data, changed_sector_list);
    
    // FIXME - split data up if not enough space for just one extent
    
    if (start_data > 0) {
        try_extend_data(Vcb, fcb, start_data, length, data, changed_sector_list, Irp, &written, rollback);
        
        if (written == length)
            return STATUS_SUCCESS;
        else if (written > 0) {
            start_data += written;
            length -= written;
            data = &((UINT8*)data)[written];
        }
    }
    
    flags = Vcb->data_flags;
    
    while (written < orig_length) {
        UINT64 newlen = min(length, MAX_EXTENT_SIZE);
        BOOL done = FALSE;
        
        // Rather than necessarily writing the whole extent at once, we deal with it in blocks of 128 MB.
        // First, see if we can write the extent part to an existing chunk.
        
        ExAcquireResourceSharedLite(&Vcb->chunk_lock, TRUE);
        
        le = Vcb->chunks.Flink;
        while (le != &Vcb->chunks) {
            c = CONTAINING_RECORD(le, chunk, list_entry);
            
            ExAcquireResourceExclusiveLite(&c->lock, TRUE);
            
            if (c->chunk_item->type == flags && (c->chunk_item->size - c->used) >= newlen) {
                if (insert_extent_chunk(Vcb, fcb, c, start_data, newlen, FALSE, data, changed_sector_list, Irp, rollback, BTRFS_COMPRESSION_NONE, newlen)) {
                    written += newlen;
                    
                    if (written == orig_length) {
                        ExReleaseResourceLite(&c->lock);
                        ExReleaseResourceLite(&Vcb->chunk_lock);
                        return STATUS_SUCCESS;
                    } else {
                        done = TRUE;
                        start_data += newlen;
                        length -= newlen;
                        data = &((UINT8*)data)[newlen];
                        break;
                    }
                }
            }
            
            ExReleaseResourceLite(&c->lock);

            le = le->Flink;
        }
        
        ExReleaseResourceLite(&fcb->Vcb->chunk_lock);
        
        if (done) continue;
        
        // Otherwise, see if we can put it in a new chunk.
        
        ExAcquireResourceExclusiveLite(&fcb->Vcb->chunk_lock, TRUE);
        
        if ((c = alloc_chunk(Vcb, flags))) {
            ExReleaseResourceLite(&Vcb->chunk_lock);
            
            ExAcquireResourceExclusiveLite(&c->lock, TRUE);
            
            if (c->chunk_item->type == flags && (c->chunk_item->size - c->used) >= newlen) {
                if (insert_extent_chunk(Vcb, fcb, c, start_data, newlen, FALSE, data, changed_sector_list, Irp, rollback, BTRFS_COMPRESSION_NONE, newlen)) {
                    written += newlen;
                    
                    if (written == orig_length) {
                        ExReleaseResourceLite(&c->lock);
                        return STATUS_SUCCESS;
                    } else {
                        done = TRUE;
                        start_data += newlen;
                        length -= newlen;
                        data = &((UINT8*)data)[newlen];
                    }
                }
            }
            
            ExReleaseResourceLite(&c->lock);
        } else
            ExReleaseResourceLite(&Vcb->chunk_lock);
        
        if (!done) {
            FIXME("FIXME - not enough room to write whole extent part, try to write bits and pieces\n"); // FIXME
            break;
        }
    }
    
    WARN("couldn't find any data chunks with %llx bytes free\n", length);

    return STATUS_DISK_FULL;
}

void commit_checksum_changes(device_extension* Vcb, LIST_ENTRY* changed_sector_list) {
    while (!IsListEmpty(changed_sector_list)) {
        LIST_ENTRY* le = RemoveHeadList(changed_sector_list);
        InsertTailList(&Vcb->sector_checksums, le);
    }
}

NTSTATUS truncate_file(fcb* fcb, UINT64 end, PIRP Irp, LIST_ENTRY* rollback) {
    NTSTATUS Status;
    
    // FIXME - convert into inline extent if short enough
    
    Status = excise_extents(fcb->Vcb, fcb, sector_align(end, fcb->Vcb->superblock.sector_size),
                            sector_align(fcb->inode_item.st_size, fcb->Vcb->superblock.sector_size), Irp, rollback);
    if (!NT_SUCCESS(Status)) {
        ERR("error - excise_extents failed\n");
        return Status;
    }
    
    fcb->inode_item.st_size = end;
    TRACE("setting st_size to %llx\n", end);

    fcb->Header.AllocationSize.QuadPart = sector_align(fcb->inode_item.st_size, fcb->Vcb->superblock.sector_size);
    fcb->Header.FileSize.QuadPart = fcb->inode_item.st_size;
    fcb->Header.ValidDataLength.QuadPart = fcb->inode_item.st_size;
    // FIXME - inform cache manager of this
    
    TRACE("fcb %p FileSize = %llx\n", fcb, fcb->Header.FileSize.QuadPart);
    
    return STATUS_SUCCESS;
}

NTSTATUS extend_file(fcb* fcb, file_ref* fileref, UINT64 end, BOOL prealloc, PIRP Irp, LIST_ENTRY* rollback) {
    UINT64 oldalloc, newalloc;
    BOOL cur_inline;
    NTSTATUS Status;
    
    TRACE("(%p, %p, %x, %u)\n", fcb, fileref, end, prealloc);

    if (fcb->ads)
        return stream_set_end_of_file_information(fcb->Vcb, end, fcb, fileref, NULL, FALSE, rollback);
    else {
        extent* ext = NULL;
        LIST_ENTRY* le;
        
        le = fcb->extents.Blink;
        while (le != &fcb->extents) {
            extent* ext2 = CONTAINING_RECORD(le, extent, list_entry);
            
            if (!ext2->ignore) {
                ext = ext2;
                break;
            }
            
            le = le->Blink;
        }
        
        oldalloc = 0;
        if (ext) {
            EXTENT_DATA* ed = ext->data;
            EXTENT_DATA2* ed2 = (EXTENT_DATA2*)ed->data;
            
            if (ext->datalen < sizeof(EXTENT_DATA)) {
                ERR("extent %llx was %u bytes, expected at least %u\n", ext->offset, ext->datalen, sizeof(EXTENT_DATA));
                return STATUS_INTERNAL_ERROR;
            }
            
            oldalloc = ext->offset + (ed->type == EXTENT_TYPE_INLINE ? ed->decoded_size : ed2->num_bytes);
            cur_inline = ed->type == EXTENT_TYPE_INLINE;
        
            if (cur_inline && end > fcb->Vcb->options.max_inline) {
                LIST_ENTRY changed_sector_list;
                BOOL nocsum = fcb->inode_item.flags & BTRFS_INODE_NODATASUM;
                UINT64 origlength, length;
                UINT8* data;
                UINT64 offset = ext->offset;
                
                TRACE("giving inline file proper extents\n");
                
                origlength = ed->decoded_size;
                
                cur_inline = FALSE;
                
                if (!nocsum)
                    InitializeListHead(&changed_sector_list);
                
                length = sector_align(origlength, fcb->Vcb->superblock.sector_size);
                
                data = ExAllocatePoolWithTag(PagedPool, length, ALLOC_TAG);
                if (!data) {
                    ERR("could not allocate %llx bytes for data\n", length);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                
                if (length > origlength)
                    RtlZeroMemory(data + origlength, length - origlength);
                
                RtlCopyMemory(data, ed->data, origlength);
                
                fcb->inode_item.st_blocks -= origlength;
                
                remove_fcb_extent(fcb, ext, rollback);
                
                if (write_fcb_compressed(fcb)) {
                    Status = write_compressed(fcb, offset, offset + length, data, nocsum ? NULL : &changed_sector_list, Irp, rollback);
                    if (!NT_SUCCESS(Status)) {
                        ERR("write_compressed returned %08x\n", Status);
                        ExFreePool(data);
                        return Status;
                    }
                } else {
                    Status = insert_extent(fcb->Vcb, fcb, offset, length, data, nocsum ? NULL : &changed_sector_list, Irp, rollback);
                    if (!NT_SUCCESS(Status)) {
                        ERR("insert_extent returned %08x\n", Status);
                        ExFreePool(data);
                        return Status;
                    }
                }
                
                oldalloc = ext->offset + length;
                
                ExFreePool(data);
                
                if (!nocsum) {
                    ExAcquireResourceExclusiveLite(&fcb->Vcb->checksum_lock, TRUE);
                    commit_checksum_changes(fcb->Vcb, &changed_sector_list);
                    ExReleaseResourceLite(&fcb->Vcb->checksum_lock);
                }
            }
            
            if (cur_inline) {
                ULONG edsize;
                
                if (end > oldalloc) {
                    edsize = sizeof(EXTENT_DATA) - 1 + end - ext->offset;
                    ed = ExAllocatePoolWithTag(PagedPool, edsize, ALLOC_TAG);
                    
                    if (!ed) {
                        ERR("out of memory\n");
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }
                    
                    RtlZeroMemory(ed, edsize);
                    RtlCopyMemory(ed, ext->data, ext->datalen);
                    
                    ed->decoded_size = end - ext->offset;
                    
                    remove_fcb_extent(fcb, ext, rollback);
                    
                    if (!add_extent_to_fcb(fcb, ext->offset, ed, edsize, ext->unique, rollback)) {
                        ERR("add_extent_to_fcb failed\n");
                        ExFreePool(ed);
                        return STATUS_INTERNAL_ERROR;
                    }
                    
                    fcb->extents_changed = TRUE;
                    mark_fcb_dirty(fcb);
                }
                
                TRACE("extending inline file (oldalloc = %llx, end = %llx)\n", oldalloc, end);
                
                fcb->inode_item.st_size = end;
                TRACE("setting st_size to %llx\n", end);
                
                fcb->inode_item.st_blocks = end;

                fcb->Header.AllocationSize.QuadPart = fcb->Header.FileSize.QuadPart = fcb->Header.ValidDataLength.QuadPart = end;
            } else {
                newalloc = sector_align(end, fcb->Vcb->superblock.sector_size);
            
                if (newalloc > oldalloc) {
                    if (prealloc) {
                        // FIXME - try and extend previous extent first
                        
                        Status = insert_prealloc_extent(fcb, oldalloc, newalloc - oldalloc, rollback);
                    
                        if (!NT_SUCCESS(Status)) {
                            ERR("insert_prealloc_extent returned %08x\n", Status);
                            return Status;
                        }
                    }
                    
                    fcb->extents_changed = TRUE;
                    mark_fcb_dirty(fcb);
                }
                
                fcb->inode_item.st_size = end;
                TRACE("setting st_size to %llx\n", end);
                
                TRACE("newalloc = %llx\n", newalloc);
                
                fcb->Header.AllocationSize.QuadPart = newalloc;
                fcb->Header.FileSize.QuadPart = fcb->Header.ValidDataLength.QuadPart = end;
            }
        } else {
            if (end > fcb->Vcb->options.max_inline) {
                newalloc = sector_align(end, fcb->Vcb->superblock.sector_size);
            
                if (prealloc) {
                    Status = insert_prealloc_extent(fcb, 0, newalloc, rollback);
                    
                    if (!NT_SUCCESS(Status)) {
                        ERR("insert_prealloc_extent returned %08x\n", Status);
                        return Status;
                    }
                }
                
                fcb->extents_changed = TRUE;
                mark_fcb_dirty(fcb);
                
                fcb->inode_item.st_size = end;
                TRACE("setting st_size to %llx\n", end);
                
                TRACE("newalloc = %llx\n", newalloc);
                
                fcb->Header.AllocationSize.QuadPart = newalloc;
                fcb->Header.FileSize.QuadPart = fcb->Header.ValidDataLength.QuadPart = end;
            } else {
                EXTENT_DATA* ed;
                ULONG edsize;
                
                edsize = sizeof(EXTENT_DATA) - 1 + end;
                ed = ExAllocatePoolWithTag(PagedPool, edsize, ALLOC_TAG);
                
                if (!ed) {
                    ERR("out of memory\n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                
                ed->generation = fcb->Vcb->superblock.generation;
                ed->decoded_size = end;
                ed->compression = BTRFS_COMPRESSION_NONE;
                ed->encryption = BTRFS_ENCRYPTION_NONE;
                ed->encoding = BTRFS_ENCODING_NONE;
                ed->type = EXTENT_TYPE_INLINE;
                
                RtlZeroMemory(ed->data, end);
                
                if (!add_extent_to_fcb(fcb, 0, ed, edsize, FALSE, rollback)) {
                    ERR("add_extent_to_fcb failed\n");
                    ExFreePool(ed);
                    return STATUS_INTERNAL_ERROR;
                }
                
                fcb->extents_changed = TRUE;
                mark_fcb_dirty(fcb);
                
                fcb->inode_item.st_size = end;
                TRACE("setting st_size to %llx\n", end);
                
                fcb->inode_item.st_blocks = end;

                fcb->Header.AllocationSize.QuadPart = fcb->Header.FileSize.QuadPart = fcb->Header.ValidDataLength.QuadPart = end;
            }
        }
    }
    
    return STATUS_SUCCESS;
}

static NTSTATUS do_write_file_prealloc(fcb* fcb, extent* ext, UINT64 start_data, UINT64 end_data, void* data, UINT64* written,
                                       LIST_ENTRY* changed_sector_list, PIRP Irp, LIST_ENTRY* rollback) {
    EXTENT_DATA* ed = ext->data;
    EXTENT_DATA2* ed2 = (EXTENT_DATA2*)ed->data;
    NTSTATUS Status;
    chunk* c;
    
    if (start_data <= ext->offset && end_data >= ext->offset + ed2->num_bytes) { // replace all
        EXTENT_DATA* ned;
        extent* newext;
        
        ned = ExAllocatePoolWithTag(PagedPool, ext->datalen, ALLOC_TAG);
        if (!ned) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        newext = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
        if (!newext) {
            ERR("out of memory\n");
            ExFreePool(ned);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        RtlCopyMemory(ned, ext->data, ext->datalen);
        
        ned->type = EXTENT_TYPE_REGULAR;
        
        Status = do_write_data(fcb->Vcb, ed2->address + ed2->offset, (UINT8*)data + ext->offset - start_data, ed2->num_bytes, changed_sector_list, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("do_write_data returned %08x\n", Status);
            return Status;
        }
        
        *written = ed2->num_bytes;
        
        newext->offset = ext->offset;
        newext->data = ned;
        newext->datalen = ext->datalen;
        newext->unique = ext->unique;
        newext->ignore = FALSE;
        InsertHeadList(&ext->list_entry, &newext->list_entry);

        add_insert_extent_rollback(rollback, fcb, newext);
        
        remove_fcb_extent(fcb, ext, rollback);
    } else if (start_data <= ext->offset && end_data < ext->offset + ed2->num_bytes) { // replace beginning
        EXTENT_DATA *ned, *nedb;
        EXTENT_DATA2* ned2;
        extent *newext1, *newext2;
        
        ned = ExAllocatePoolWithTag(PagedPool, ext->datalen, ALLOC_TAG);
        if (!ned) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        nedb = ExAllocatePoolWithTag(PagedPool, ext->datalen, ALLOC_TAG);
        if (!nedb) {
            ERR("out of memory\n");
            ExFreePool(ned);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        newext1 = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
        if (!newext1) {
            ERR("out of memory\n");
            ExFreePool(ned);
            ExFreePool(nedb);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        newext2 = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
        if (!newext2) {
            ERR("out of memory\n");
            ExFreePool(ned);
            ExFreePool(nedb);
            ExFreePool(newext1);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        RtlCopyMemory(ned, ext->data, ext->datalen);
        ned->type = EXTENT_TYPE_REGULAR;
        ned2 = (EXTENT_DATA2*)ned->data;
        ned2->num_bytes = end_data - ext->offset;
        
        RtlCopyMemory(nedb, ext->data, ext->datalen);
        ned2 = (EXTENT_DATA2*)nedb->data;
        ned2->offset += end_data - ext->offset;
        ned2->num_bytes -= end_data - ext->offset;
        
        Status = do_write_data(fcb->Vcb, ed2->address + ed2->offset, (UINT8*)data + ext->offset - start_data, end_data - ext->offset, changed_sector_list, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("do_write_data returned %08x\n", Status);
            return Status;
        }
        
        *written = end_data - ext->offset;
        
        newext1->offset = ext->offset;
        newext1->data = ned;
        newext1->datalen = ext->datalen;
        newext1->unique = FALSE;
        newext1->ignore = FALSE;
        InsertHeadList(&ext->list_entry, &newext1->list_entry);
        
        add_insert_extent_rollback(rollback, fcb, newext1);
        
        newext2->offset = end_data;
        newext2->data = nedb;
        newext2->datalen = ext->datalen;
        newext2->unique = FALSE;
        newext2->ignore = FALSE;
        InsertHeadList(&newext1->list_entry, &newext2->list_entry);
        
        add_insert_extent_rollback(rollback, fcb, newext2);
        
        c = get_chunk_from_address(fcb->Vcb, ed2->address);
        
        if (!c)
            ERR("get_chunk_from_address(%llx) failed\n", ed2->address);
        else {
            Status = update_changed_extent_ref(fcb->Vcb, c, ed2->address, ed2->size, fcb->subvol->id, fcb->inode, ext->offset - ed2->offset, 1,
                                                fcb->inode_item.flags & BTRFS_INODE_NODATASUM, ed2->size, Irp);
            
            if (!NT_SUCCESS(Status)) {
                ERR("update_changed_extent_ref returned %08x\n", Status);
                return Status;
            }
        }

        remove_fcb_extent(fcb, ext, rollback);
    } else if (start_data > ext->offset && end_data >= ext->offset + ed2->num_bytes) { // replace end
        EXTENT_DATA *ned, *nedb;
        EXTENT_DATA2* ned2;
        extent *newext1, *newext2;
        
        ned = ExAllocatePoolWithTag(PagedPool, ext->datalen, ALLOC_TAG);
        if (!ned) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        nedb = ExAllocatePoolWithTag(PagedPool, ext->datalen, ALLOC_TAG);
        if (!nedb) {
            ERR("out of memory\n");
            ExFreePool(ned);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        newext1 = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
        if (!newext1) {
            ERR("out of memory\n");
            ExFreePool(ned);
            ExFreePool(nedb);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        newext2 = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
        if (!newext2) {
            ERR("out of memory\n");
            ExFreePool(ned);
            ExFreePool(nedb);
            ExFreePool(newext1);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        RtlCopyMemory(ned, ext->data, ext->datalen);
        
        ned2 = (EXTENT_DATA2*)ned->data;
        ned2->num_bytes = start_data - ext->offset;
        
        RtlCopyMemory(nedb, ext->data, ext->datalen);
        
        nedb->type = EXTENT_TYPE_REGULAR;
        ned2 = (EXTENT_DATA2*)nedb->data;
        ned2->offset += start_data - ext->offset;
        ned2->num_bytes = ext->offset + ed2->num_bytes - start_data;
        
        Status = do_write_data(fcb->Vcb, ed2->address + ned2->offset, data, ned2->num_bytes, changed_sector_list, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("do_write_data returned %08x\n", Status);
            return Status;
        }
        
        *written = ned2->num_bytes;
        
        newext1->offset = ext->offset;
        newext1->data = ned;
        newext1->datalen = ext->datalen;
        newext1->unique = FALSE;
        newext1->ignore = FALSE;
        InsertHeadList(&ext->list_entry, &newext1->list_entry);
        
        add_insert_extent_rollback(rollback, fcb, newext1);
        
        newext2->offset = start_data;
        newext2->data = nedb;
        newext2->datalen = ext->datalen;
        newext2->unique = FALSE;
        newext2->ignore = FALSE;
        InsertHeadList(&newext1->list_entry, &newext2->list_entry);
        
        add_insert_extent_rollback(rollback, fcb, newext2);
        
        c = get_chunk_from_address(fcb->Vcb, ed2->address);
        
        if (!c)
            ERR("get_chunk_from_address(%llx) failed\n", ed2->address);
        else {
            Status = update_changed_extent_ref(fcb->Vcb, c, ed2->address, ed2->size, fcb->subvol->id, fcb->inode, ext->offset - ed2->offset, 1,
                                               fcb->inode_item.flags & BTRFS_INODE_NODATASUM, ed2->size, Irp);
            
            if (!NT_SUCCESS(Status)) {
                ERR("update_changed_extent_ref returned %08x\n", Status);
                return Status;
            }
        }

        remove_fcb_extent(fcb, ext, rollback);
    } else if (start_data > ext->offset && end_data < ext->offset + ed2->num_bytes) { // replace middle
        EXTENT_DATA *ned, *nedb, *nedc;
        EXTENT_DATA2* ned2;
        extent *newext1, *newext2, *newext3;
        
        ned = ExAllocatePoolWithTag(PagedPool, ext->datalen, ALLOC_TAG);
        if (!ned) {
            ERR("out of memory\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        nedb = ExAllocatePoolWithTag(PagedPool, ext->datalen, ALLOC_TAG);
        if (!nedb) {
            ERR("out of memory\n");
            ExFreePool(ned);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        nedc = ExAllocatePoolWithTag(PagedPool, ext->datalen, ALLOC_TAG);
        if (!nedb) {
            ERR("out of memory\n");
            ExFreePool(ned);
            ExFreePool(nedb);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        newext1 = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
        if (!newext1) {
            ERR("out of memory\n");
            ExFreePool(ned);
            ExFreePool(nedb);
            ExFreePool(nedc);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        newext2 = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
        if (!newext2) {
            ERR("out of memory\n");
            ExFreePool(ned);
            ExFreePool(nedb);
            ExFreePool(nedc);
            ExFreePool(newext1);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        newext3 = ExAllocatePoolWithTag(PagedPool, sizeof(extent), ALLOC_TAG);
        if (!newext2) {
            ERR("out of memory\n");
            ExFreePool(ned);
            ExFreePool(nedb);
            ExFreePool(nedc);
            ExFreePool(newext1);
            ExFreePool(newext2);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        
        RtlCopyMemory(ned, ext->data, ext->datalen);
        RtlCopyMemory(nedb, ext->data, ext->datalen);
        RtlCopyMemory(nedc, ext->data, ext->datalen);
        
        ned2 = (EXTENT_DATA2*)ned->data;
        ned2->num_bytes = start_data - ext->offset;
        
        nedb->type = EXTENT_TYPE_REGULAR;
        ned2 = (EXTENT_DATA2*)nedb->data;
        ned2->offset += start_data - ext->offset;
        ned2->num_bytes = end_data - start_data;
        
        ned2 = (EXTENT_DATA2*)nedc->data;
        ned2->offset += end_data - ext->offset;
        ned2->num_bytes -= end_data - ext->offset;
        
        ned2 = (EXTENT_DATA2*)nedb->data;
        Status = do_write_data(fcb->Vcb, ed2->address + ned2->offset, data, end_data - start_data, changed_sector_list, Irp);
        if (!NT_SUCCESS(Status)) {
            ERR("do_write_data returned %08x\n", Status);
            return Status;
        }
        
        *written = end_data - start_data;
        
        newext1->offset = ext->offset;
        newext1->data = ned;
        newext1->datalen = ext->datalen;
        newext1->unique = FALSE;
        newext1->ignore = FALSE;
        InsertHeadList(&ext->list_entry, &newext1->list_entry);
        
        add_insert_extent_rollback(rollback, fcb, newext1);
        
        newext2->offset = start_data;
        newext2->data = nedb;
        newext2->datalen = ext->datalen;
        newext2->unique = FALSE;
        newext2->ignore = FALSE;
        InsertHeadList(&newext1->list_entry, &newext2->list_entry);
        
        add_insert_extent_rollback(rollback, fcb, newext2);
        
        newext3->offset = end_data;
        newext3->data = nedc;
        newext3->datalen = ext->datalen;
        newext3->unique = FALSE;
        newext3->ignore = FALSE;
        InsertHeadList(&newext2->list_entry, &newext3->list_entry);
        
        add_insert_extent_rollback(rollback, fcb, newext3);
        
        c = get_chunk_from_address(fcb->Vcb, ed2->address);
        
        if (!c)
            ERR("get_chunk_from_address(%llx) failed\n", ed2->address);
        else {
            Status = update_changed_extent_ref(fcb->Vcb, c, ed2->address, ed2->size, fcb->subvol->id, fcb->inode, ext->offset - ed2->offset, 2,
                                               fcb->inode_item.flags & BTRFS_INODE_NODATASUM, ed2->size, Irp);
            
            if (!NT_SUCCESS(Status)) {
                ERR("update_changed_extent_ref returned %08x\n", Status);
                return Status;
            }
        }

        remove_fcb_extent(fcb, ext, rollback);
    }
    
    return STATUS_SUCCESS;
}

NTSTATUS do_write_file(fcb* fcb, UINT64 start, UINT64 end_data, void* data, LIST_ENTRY* changed_sector_list, PIRP Irp, LIST_ENTRY* rollback) {
    NTSTATUS Status;
    LIST_ENTRY *le, *le2;
    UINT64 written = 0, length = end_data - start;
    UINT64 last_cow_start;
#ifdef DEBUG_PARANOID
    UINT64 last_off;
#endif
    
    last_cow_start = 0;
    
    le = fcb->extents.Flink;
    while (le != &fcb->extents) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);
        
        le2 = le->Flink;
        
        if (!ext->ignore) {
            EXTENT_DATA* ed = ext->data;
            EXTENT_DATA2* ed2 = ed->type == EXTENT_TYPE_INLINE ? NULL : (EXTENT_DATA2*)ed->data;
            UINT64 len;
            BOOL nocow;
            
            len = ed->type == EXTENT_TYPE_INLINE ? ed->decoded_size : ed2->num_bytes;
            
            if (ext->offset + len <= start)
                goto nextitem;
            
            if (ext->offset > start + written + length)
                break;
            
            nocow = (ext->unique && fcb->inode_item.flags & BTRFS_INODE_NODATACOW) || ed->type == EXTENT_TYPE_PREALLOC;
           
            if (nocow) {
                if (max(last_cow_start, start + written) < ext->offset) {
                    UINT64 start_write = max(last_cow_start, start + written);
                    
                    Status = excise_extents(fcb->Vcb, fcb, start_write, ext->offset, Irp, rollback);
                    if (!NT_SUCCESS(Status)) {
                        ERR("excise_extents returned %08x\n", Status);
                        return Status;
                    }
                    
                    Status = insert_extent(fcb->Vcb, fcb, start_write, ext->offset - start_write, data, changed_sector_list, Irp, rollback);
                    if (!NT_SUCCESS(Status)) {
                        ERR("insert_extent returned %08x\n", Status);
                        return Status;
                    }
                    
                    written += ext->offset - start_write;
                    length -= ext->offset - start_write;
                    
                    if (length == 0)
                        break;
                }
                
                if (ed->type == EXTENT_TYPE_REGULAR) {
                    UINT64 writeaddr = ed2->address + ed2->offset + start + written - ext->offset;
                    UINT64 write_len = min(len, length);
                                    
                    TRACE("doing non-COW write to %llx\n", writeaddr);
                    
                    Status = write_data_complete(fcb->Vcb, writeaddr, (UINT8*)data + written, write_len, Irp, NULL);
                    if (!NT_SUCCESS(Status)) {
                        ERR("write_data_complete returned %08x\n", Status);
                        return Status;
                    }
                    
                    if (changed_sector_list) {
                        unsigned int i;
                        changed_sector* sc;
                        
                        sc = ExAllocatePoolWithTag(PagedPool, sizeof(changed_sector), ALLOC_TAG);
                        if (!sc) {
                            ERR("out of memory\n");
                            return STATUS_INSUFFICIENT_RESOURCES;
                        }
                        
                        sc->ol.key = writeaddr;
                        sc->length = write_len / fcb->Vcb->superblock.sector_size;
                        sc->deleted = FALSE;
                        
                        sc->checksums = ExAllocatePoolWithTag(PagedPool, sizeof(UINT32) * sc->length, ALLOC_TAG);
                        if (!sc->checksums) {
                            ERR("out of memory\n");
                            ExFreePool(sc);
                            return STATUS_INSUFFICIENT_RESOURCES;
                        }
                        
                        for (i = 0; i < sc->length; i++) {
                            sc->checksums[i] = ~calc_crc32c(0xffffffff, (UINT8*)data + written + (i * fcb->Vcb->superblock.sector_size), fcb->Vcb->superblock.sector_size);
                        }
    
                        insert_into_ordered_list(changed_sector_list, &sc->ol);
                    }
                    
                    written += write_len;
                    length -= write_len;
                    
                    if (length == 0)
                        break;
                } else if (ed->type == EXTENT_TYPE_PREALLOC) {
                    UINT64 write_len;
                    
                    Status = do_write_file_prealloc(fcb, ext, start + written, end_data, (UINT8*)data + written, &write_len,
                                                    changed_sector_list, Irp, rollback);
                    if (!NT_SUCCESS(Status)) {
                        ERR("do_write_file_prealloc returned %08x\n", Status);
                        return Status;
                    }
                    
                    written += write_len;
                    length -= write_len;
                    
                    if (length == 0)
                        break;
                }
                
                last_cow_start = ext->offset + len;
            }
        }
        
nextitem:
        le = le2;
    }
    
    if (length > 0) {
        UINT64 start_write = max(last_cow_start, start + written);
        
        Status = excise_extents(fcb->Vcb, fcb, start_write, end_data, Irp, rollback);
        if (!NT_SUCCESS(Status)) {
            ERR("excise_extents returned %08x\n", Status);
            return Status;
        }
        
        Status = insert_extent(fcb->Vcb, fcb, start_write, end_data - start_write, data, changed_sector_list, Irp, rollback);
        if (!NT_SUCCESS(Status)) {
            ERR("insert_extent returned %08x\n", Status);
            return Status;
        }
    }
    
    // FIXME - make extending work again (here?)
    // FIXME - make maximum extent size 128 MB again (here?)
    
#ifdef DEBUG_PARANOID
    last_off = 0xffffffffffffffff;
    
    le = fcb->extents.Flink;
    while (le != &fcb->extents) {
        extent* ext = CONTAINING_RECORD(le, extent, list_entry);
        
        if (!ext->ignore) {
            if (ext->offset == last_off) {
                ERR("offset %llx duplicated\n", ext->offset);
                int3;
            } else if (ext->offset < last_off && last_off != 0xffffffffffffffff) {
                ERR("offsets out of order\n");
                int3;
            }
            
            last_off = ext->offset;
        }
        
        le = le->Flink;
    }
#endif
    
    fcb->extents_changed = TRUE;
    mark_fcb_dirty(fcb);
    
    return STATUS_SUCCESS;
}

NTSTATUS write_compressed(fcb* fcb, UINT64 start_data, UINT64 end_data, void* data, LIST_ENTRY* changed_sector_list, PIRP Irp, LIST_ENTRY* rollback) {
    NTSTATUS Status;
    UINT64 i;
    
    for (i = 0; i < sector_align(end_data - start_data, COMPRESSED_EXTENT_SIZE) / COMPRESSED_EXTENT_SIZE; i++) {
        UINT64 s2, e2;
        BOOL compressed;
        
        s2 = start_data + (i * COMPRESSED_EXTENT_SIZE);
        e2 = min(s2 + COMPRESSED_EXTENT_SIZE, end_data);
        
        Status = write_compressed_bit(fcb, s2, e2, (UINT8*)data + (i * COMPRESSED_EXTENT_SIZE), &compressed, changed_sector_list, Irp, rollback);
        
        if (!NT_SUCCESS(Status)) {
            ERR("write_compressed_bit returned %08x\n", Status);
            return Status;
        }
        
        // If the first 128 KB of a file is incompressible, we set the nocompress flag so we don't
        // bother with the rest of it.
        if (s2 == 0 && e2 == COMPRESSED_EXTENT_SIZE && !compressed && !fcb->Vcb->options.compress_force) {
            fcb->inode_item.flags |= BTRFS_INODE_NOCOMPRESS;
            mark_fcb_dirty(fcb);
            
            // write subsequent data non-compressed
            if (e2 < end_data) {
                Status = do_write_file(fcb, e2, end_data, (UINT8*)data + e2, changed_sector_list, Irp, rollback);
                
                if (!NT_SUCCESS(Status)) {
                    ERR("do_write_file returned %08x\n", Status);
                    return Status;
                }
            }
            
            return STATUS_SUCCESS;
        }
    }
    
    return STATUS_SUCCESS;
}

NTSTATUS write_file2(device_extension* Vcb, PIRP Irp, LARGE_INTEGER offset, void* buf, ULONG* length, BOOL paging_io, BOOL no_cache,
                     BOOL wait, BOOL deferred_write, LIST_ENTRY* rollback) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    EXTENT_DATA* ed2;
    UINT64 newlength, start_data, end_data;
    UINT32 bufhead;
    BOOL make_inline;
    UINT8* data;
    LIST_ENTRY changed_sector_list;
    INODE_ITEM* origii;
    BOOL changed_length = FALSE, nocsum/*, lazy_writer = FALSE, write_eof = FALSE*/;
    NTSTATUS Status;
    LARGE_INTEGER time;
    BTRFS_TIME now;
    fcb* fcb;
    ccb* ccb;
    file_ref* fileref;
    BOOL paging_lock = FALSE, fcb_lock = FALSE, tree_lock = FALSE, pagefile;
    ULONG filter = 0;
    
    TRACE("(%p, %p, %llx, %p, %x, %u, %u)\n", Vcb, FileObject, offset.QuadPart, buf, *length, paging_io, no_cache);
    
    if (*length == 0) {
        WARN("returning success for zero-length write\n");
        return STATUS_SUCCESS;
    }
    
    if (!FileObject) {
        ERR("error - FileObject was NULL\n");
        return STATUS_ACCESS_DENIED;
    }
    
    fcb = FileObject->FsContext;
    ccb = FileObject->FsContext2;
    fileref = ccb ? ccb->fileref : NULL;
    
    if (fcb->type != BTRFS_TYPE_FILE && fcb->type != BTRFS_TYPE_SYMLINK) {
        WARN("tried to write to something other than a file or symlink (inode %llx, type %u, %p, %p)\n", fcb->inode, fcb->type, &fcb->type, fcb);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    
    if (offset.LowPart == FILE_WRITE_TO_END_OF_FILE && offset.HighPart == -1) {
        offset = fcb->Header.FileSize;
//         write_eof = TRUE;
    }
    
    TRACE("fcb->Header.Flags = %x\n", fcb->Header.Flags);
    
    if (!no_cache && !CcCanIWrite(FileObject, *length, wait, deferred_write))
        return STATUS_PENDING;
    
    if (!wait && no_cache)
        return STATUS_PENDING;
    
    if (no_cache && !paging_io && FileObject->SectionObjectPointer->DataSectionObject) {
        IO_STATUS_BLOCK iosb;
        
        ExAcquireResourceExclusiveLite(fcb->Header.PagingIoResource, TRUE);

        CcFlushCache(FileObject->SectionObjectPointer, &offset, *length, &iosb);

        if (!NT_SUCCESS(iosb.Status)) {
            ExReleaseResourceLite(fcb->Header.PagingIoResource);
            ERR("CcFlushCache returned %08x\n", iosb.Status);
            return iosb.Status;
        }
        
        paging_lock = TRUE;

        CcPurgeCacheSection(FileObject->SectionObjectPointer, &offset, *length, FALSE);
    }
    
    if (paging_io) {
        if (!ExAcquireResourceSharedLite(fcb->Header.PagingIoResource, wait)) {
            Status = STATUS_PENDING;
            goto end;
        } else
            paging_lock = TRUE;
    }
    
    pagefile = fcb->Header.Flags2 & FSRTL_FLAG2_IS_PAGING_FILE && paging_io;
    
    if (!pagefile && !ExIsResourceAcquiredExclusiveLite(&Vcb->tree_lock)) {
        if (!ExAcquireResourceSharedLite(&Vcb->tree_lock, wait)) {
            Status = STATUS_PENDING;
            goto end;
        } else
            tree_lock = TRUE;
    }
        
    if (no_cache && !ExIsResourceAcquiredExclusiveLite(fcb->Header.Resource)) {
        if (!ExAcquireResourceExclusiveLite(fcb->Header.Resource, wait)) {
            Status = STATUS_PENDING;
            goto end;
        } else
            fcb_lock = TRUE;
    }
    
    nocsum = fcb->ads ? TRUE : fcb->inode_item.flags & BTRFS_INODE_NODATASUM;
    
    newlength = fcb->ads ? fcb->adsdata.Length : fcb->inode_item.st_size;
    
    if (fcb->deleted)
        newlength = 0;
    
    TRACE("newlength = %llx\n", newlength);
    
//     if (KeGetCurrentThread() == fcb->lazy_writer_thread) {
//         ERR("lazy writer on the TV\n");
//         lazy_writer = TRUE;
//     }
    
    if (offset.QuadPart + *length > newlength) {
        if (paging_io) {
            if (offset.QuadPart >= newlength) {
                TRACE("paging IO tried to write beyond end of file (file size = %llx, offset = %llx, length = %x)\n", newlength, offset.QuadPart, *length);
                TRACE("filename %S\n", file_desc(FileObject));
                TRACE("FileObject: AllocationSize = %llx, FileSize = %llx, ValidDataLength = %llx\n",
                    fcb->Header.AllocationSize.QuadPart, fcb->Header.FileSize.QuadPart, fcb->Header.ValidDataLength.QuadPart);
                Status = STATUS_SUCCESS;
                goto end;
            }
            
            *length = newlength - offset.QuadPart;
        } else {
            newlength = offset.QuadPart + *length;
            changed_length = TRUE;
            
            TRACE("extending length to %llx\n", newlength);
        }
    }
    
    make_inline = fcb->ads ? FALSE : newlength <= fcb->Vcb->options.max_inline;
    
    if (changed_length) {
        if (newlength > fcb->Header.AllocationSize.QuadPart) {
            if (!tree_lock) {
                // We need to acquire the tree lock if we don't have it already - 
                // we can't give an inline file proper extents at the same as we're
                // doing a flush.
                if (!ExAcquireResourceSharedLite(&Vcb->tree_lock, wait)) {
                    Status = STATUS_PENDING;
                    goto end;
                } else
                    tree_lock = TRUE;
            }
            
            Status = extend_file(fcb, fileref, newlength, FALSE, Irp, rollback);
            if (!NT_SUCCESS(Status)) {
                ERR("extend_file returned %08x\n", Status);
                goto end;
            }
        } else if (!fcb->ads)
            fcb->inode_item.st_size = newlength;
        
        fcb->Header.FileSize.QuadPart = newlength;
        fcb->Header.ValidDataLength.QuadPart = newlength;
        
        TRACE("AllocationSize = %llx\n", fcb->Header.AllocationSize.QuadPart);
        TRACE("FileSize = %llx\n", fcb->Header.FileSize.QuadPart);
        TRACE("ValidDataLength = %llx\n", fcb->Header.ValidDataLength.QuadPart);
    }
    
    if (!no_cache) {
        if (!FileObject->PrivateCacheMap || changed_length) {
            CC_FILE_SIZES ccfs;
            
            ccfs.AllocationSize = fcb->Header.AllocationSize;
            ccfs.FileSize = fcb->Header.FileSize;
            ccfs.ValidDataLength = fcb->Header.ValidDataLength;
            
            if (!FileObject->PrivateCacheMap) {
                TRACE("calling CcInitializeCacheMap...\n");
                CcInitializeCacheMap(FileObject, &ccfs, FALSE, cache_callbacks, FileObject);
                
                CcSetReadAheadGranularity(FileObject, READ_AHEAD_GRANULARITY);
            }
            
            CcSetFileSizes(FileObject, &ccfs);
        }
        
        if (IrpSp->MinorFunction & IRP_MN_MDL) {
            CcPrepareMdlWrite(FileObject, &offset, *length, &Irp->MdlAddress, &Irp->IoStatus);

            Status = Irp->IoStatus.Status;
            goto end;
        } else {
            TRACE("CcCopyWrite(%p, %llx, %x, %u, %p)\n", FileObject, offset.QuadPart, *length, wait, buf);
            if (!CcCopyWrite(FileObject, &offset, *length, wait, buf)) {
                Status = STATUS_PENDING;
                goto end;
            }
            TRACE("CcCopyWrite finished\n");
        }
        
        Status = STATUS_SUCCESS;
        goto end;
    }
    
    if (fcb->ads) {
        if (changed_length) {
            char* data2;
            
            if (newlength > fcb->adsmaxlen) {
                ERR("error - xattr too long (%llu > %u)\n", newlength, fcb->adsmaxlen);
                Status = STATUS_DISK_FULL;
                goto end;
            }

            data2 = ExAllocatePoolWithTag(PagedPool, newlength, ALLOC_TAG);
            if (!data2) {
                ERR("out of memory\n");
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto end;
            }
            
            if (fcb->adsdata.Buffer) {
                RtlCopyMemory(data2, fcb->adsdata.Buffer, fcb->adsdata.Length);
                ExFreePool(fcb->adsdata.Buffer);
            }
            
            if (newlength > fcb->adsdata.Length)
                RtlZeroMemory(&data2[fcb->adsdata.Length], newlength - fcb->adsdata.Length);
            
            
            fcb->adsdata.Buffer = data2;
            fcb->adsdata.Length = fcb->adsdata.MaximumLength = newlength;
            
            fcb->Header.AllocationSize.QuadPart = newlength;
            fcb->Header.FileSize.QuadPart = newlength;
            fcb->Header.ValidDataLength.QuadPart = newlength;
        }
        
        if (*length > 0)
            RtlCopyMemory(&fcb->adsdata.Buffer[offset.QuadPart], buf, *length);
        
        fcb->Header.ValidDataLength.QuadPart = newlength;
        
        mark_fcb_dirty(fcb);
        
        if (fileref)
            mark_fileref_dirty(fileref);
    } else {
        BOOL compress = write_fcb_compressed(fcb);
        
        if (make_inline) {
            start_data = 0;
            end_data = sector_align(newlength, fcb->Vcb->superblock.sector_size);
            bufhead = sizeof(EXTENT_DATA) - 1;
        } else if (compress) {
            start_data = offset.QuadPart & ~(UINT64)(COMPRESSED_EXTENT_SIZE - 1);
            end_data = min(sector_align(offset.QuadPart + *length, COMPRESSED_EXTENT_SIZE),
                           sector_align(newlength, fcb->Vcb->superblock.sector_size));
            bufhead = 0;
        } else {
            start_data = offset.QuadPart & ~(UINT64)(fcb->Vcb->superblock.sector_size - 1);
            end_data = sector_align(offset.QuadPart + *length, fcb->Vcb->superblock.sector_size);
            bufhead = 0;
        }
            
        fcb->Header.ValidDataLength.QuadPart = newlength;
        TRACE("fcb %p FileSize = %llx\n", fcb, fcb->Header.FileSize.QuadPart);
    
        data = ExAllocatePoolWithTag(PagedPool, end_data - start_data + bufhead, ALLOC_TAG);
        if (!data) {
            ERR("out of memory\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto end;
        }
        
        RtlZeroMemory(data + bufhead, end_data - start_data);
        
        TRACE("start_data = %llx\n", start_data);
        TRACE("end_data = %llx\n", end_data);
        
        if (offset.QuadPart > start_data || offset.QuadPart + *length < end_data) {
            if (changed_length) {
                if (fcb->inode_item.st_size > start_data) 
                    Status = read_file(fcb, data + bufhead, start_data, fcb->inode_item.st_size - start_data, NULL, Irp);
                else
                    Status = STATUS_SUCCESS;
            } else
                Status = read_file(fcb, data + bufhead, start_data, end_data - start_data, NULL, Irp);
            
            if (!NT_SUCCESS(Status)) {
                ERR("read_file returned %08x\n", Status);
                ExFreePool(data);
                goto end;
            }
        }
        
        RtlCopyMemory(data + bufhead + offset.QuadPart - start_data, buf, *length);
        
        if (!nocsum)
            InitializeListHead(&changed_sector_list);

        if (make_inline) {
            Status = excise_extents(fcb->Vcb, fcb, start_data, end_data, Irp, rollback);
            if (!NT_SUCCESS(Status)) {
                ERR("error - excise_extents returned %08x\n", Status);
                ExFreePool(data);
                goto end;
            }
            
            ed2 = (EXTENT_DATA*)data;
            ed2->generation = fcb->Vcb->superblock.generation;
            ed2->decoded_size = newlength;
            ed2->compression = BTRFS_COMPRESSION_NONE;
            ed2->encryption = BTRFS_ENCRYPTION_NONE;
            ed2->encoding = BTRFS_ENCODING_NONE;
            ed2->type = EXTENT_TYPE_INLINE;
            
            if (!add_extent_to_fcb(fcb, 0, ed2, sizeof(EXTENT_DATA) - 1 + newlength, FALSE, rollback)) {
                ERR("add_extent_to_fcb failed\n");
                ExFreePool(data);
                Status = STATUS_INTERNAL_ERROR;
                goto end;
            }
            
            fcb->inode_item.st_blocks += newlength;
        } else if (compress) {
            Status = write_compressed(fcb, start_data, end_data, data, nocsum ? NULL : &changed_sector_list, Irp, rollback);
            
            if (!NT_SUCCESS(Status)) {
                ERR("write_compressed returned %08x\n", Status);
                ExFreePool(data);
                goto end;
            }
            
            ExFreePool(data);
        } else {
            Status = do_write_file(fcb, start_data, end_data, data, nocsum ? NULL : &changed_sector_list, Irp, rollback);
            
            if (!NT_SUCCESS(Status)) {
                ERR("do_write_file returned %08x\n", Status);
                ExFreePool(data);
                goto end;
            }
            
            ExFreePool(data);
        }
    }
    
    if (!pagefile) {
        KeQuerySystemTime(&time);
        win_time_to_unix(time, &now);
    
//         ERR("no_cache = %s, FileObject->PrivateCacheMap = %p\n", no_cache ? "TRUE" : "FALSE", FileObject->PrivateCacheMap);
//         
//         if (!no_cache) {
//             if (!FileObject->PrivateCacheMap) {
//                 CC_FILE_SIZES ccfs;
//                 
//                 ccfs.AllocationSize = fcb->Header.AllocationSize;
//                 ccfs.FileSize = fcb->Header.FileSize;
//                 ccfs.ValidDataLength = fcb->Header.ValidDataLength;
//                 
//                 TRACE("calling CcInitializeCacheMap...\n");
//                 CcInitializeCacheMap(FileObject, &ccfs, FALSE, cache_callbacks, fcb);
//                 
//                 changed_length = FALSE;
//             }
//         }
        
        if (fcb->ads) {
            if (fileref && fileref->parent)
                origii = &fileref->parent->fcb->inode_item;
            else {
                ERR("no parent fcb found for stream\n");
                Status = STATUS_INTERNAL_ERROR;
                goto end;
            }
        } else
            origii = &fcb->inode_item;
        
        origii->transid = Vcb->superblock.generation;
        origii->sequence++;
        origii->st_ctime = now;
        
        if (!fcb->ads) {
            if (changed_length) {
                TRACE("setting st_size to %llx\n", newlength);
                origii->st_size = newlength;
                filter |= FILE_NOTIFY_CHANGE_SIZE;
            }
            
            origii->st_mtime = now;
            filter |= FILE_NOTIFY_CHANGE_LAST_WRITE;
        }
        
        mark_fcb_dirty(fcb->ads ? fileref->parent->fcb : fcb);
    }
    
    if (!nocsum) {
        ExAcquireResourceExclusiveLite(&Vcb->checksum_lock, TRUE);
        commit_checksum_changes(Vcb, &changed_sector_list);
        ExReleaseResourceLite(&Vcb->checksum_lock);
    }
    
    if (changed_length) {
        CC_FILE_SIZES ccfs;
            
        ccfs.AllocationSize = fcb->Header.AllocationSize;
        ccfs.FileSize = fcb->Header.FileSize;
        ccfs.ValidDataLength = fcb->Header.ValidDataLength;

        CcSetFileSizes(FileObject, &ccfs);
    }
    
    // FIXME - make sure this still called if STATUS_PENDING and async
//     if (!no_cache) {
//         if (!CcCopyWrite(FileObject, &offset, *length, TRUE, buf)) {
//             ERR("CcCopyWrite failed.\n");
//         }
//     }
    
    fcb->subvol->root_item.ctransid = Vcb->superblock.generation;
    fcb->subvol->root_item.ctime = now;
    
    Status = STATUS_SUCCESS;
    
    if (filter != 0)
        send_notification_fcb(fcb->ads ? fileref->parent : fileref, filter, FILE_ACTION_MODIFIED);
    
end:
    if (NT_SUCCESS(Status) && FileObject->Flags & FO_SYNCHRONOUS_IO && !paging_io) {
        TRACE("CurrentByteOffset was: %llx\n", FileObject->CurrentByteOffset.QuadPart);
        FileObject->CurrentByteOffset.QuadPart = offset.QuadPart + (NT_SUCCESS(Status) ? *length : 0);
        TRACE("CurrentByteOffset now: %llx\n", FileObject->CurrentByteOffset.QuadPart);
    }
    
    if (fcb_lock)
        ExReleaseResourceLite(fcb->Header.Resource);
    
    if (tree_lock)
        ExReleaseResourceLite(&Vcb->tree_lock);
    
    if (paging_lock)
        ExReleaseResourceLite(fcb->Header.PagingIoResource);

    return Status;
}

NTSTATUS write_file(device_extension* Vcb, PIRP Irp, BOOL wait, BOOL deferred_write) {
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    void* buf;
    NTSTATUS Status;
    LARGE_INTEGER offset = IrpSp->Parameters.Write.ByteOffset;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    fcb* fcb = FileObject ? FileObject->FsContext : NULL;
//     BOOL locked = FALSE;
//     LARGE_INTEGER freq, time1, time2;
    LIST_ENTRY rollback;
    
    InitializeListHead(&rollback);
    
//     time1 = KeQueryPerformanceCounter(&freq);
    
    TRACE("write\n");
    
    Irp->IoStatus.Information = 0;
    
    TRACE("offset = %llx\n", offset.QuadPart);
    TRACE("length = %x\n", IrpSp->Parameters.Write.Length);
    
    if (!Irp->AssociatedIrp.SystemBuffer) {
        buf = map_user_buffer(Irp);
        
        if (Irp->MdlAddress && !buf) {
            ERR("MmGetSystemAddressForMdlSafe returned NULL\n");
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto exit;
        }   
    } else
        buf = Irp->AssociatedIrp.SystemBuffer;
    
    TRACE("buf = %p\n", buf);
    
//     if (Irp->Flags & IRP_NOCACHE) {
//         if (!ExAcquireResourceSharedLite(&Vcb->tree_lock, wait)) {
//             Status = STATUS_PENDING;
//             goto exit;
//         }
//         locked = TRUE;
//     }
    
    if (fcb && !(Irp->Flags & IRP_PAGING_IO) && !FsRtlCheckLockForWriteAccess(&fcb->lock, Irp)) {
        WARN("tried to write to locked region\n");
        Status = STATUS_FILE_LOCK_CONFLICT;
        goto exit;
    }
    
//     ERR("Irp->Flags = %x\n", Irp->Flags);
    Status = write_file2(Vcb, Irp, offset, buf, &IrpSp->Parameters.Write.Length, Irp->Flags & IRP_PAGING_IO, Irp->Flags & IRP_NOCACHE,
                         wait, deferred_write, &rollback);
    
    if (Status == STATUS_PENDING)
        goto exit;
    else if (!NT_SUCCESS(Status)) {
        ERR("write_file2 returned %08x\n", Status);
        goto exit;
    }
    
//     if (locked)
//         Status = consider_write(Vcb);

    if (NT_SUCCESS(Status)) {
        Irp->IoStatus.Information = IrpSp->Parameters.Write.Length;
    
#ifdef DEBUG_PARANOID
//         if (locked)
//             check_extents_consistent(Vcb, FileObject->FsContext); // TESTING
    
//         check_extent_tree_consistent(Vcb);
#endif
    }
    
exit:
//     if (locked) {
        if (NT_SUCCESS(Status))
            clear_rollback(&rollback);
        else
            do_rollback(Vcb, &rollback);
//         
//         ExReleaseResourceLite(&Vcb->tree_lock);
//     }
    
//     time2 = KeQueryPerformanceCounter(NULL);
    
//     ERR("time = %u (freq = %u)\n", (UINT32)(time2.QuadPart - time1.QuadPart), (UINT32)freq.QuadPart);
    
    return Status;
}

NTSTATUS STDCALL drv_write(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp) {
    NTSTATUS Status;
    BOOL top_level;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    device_extension* Vcb = DeviceObject->DeviceExtension;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    fcb* fcb = FileObject ? FileObject->FsContext : NULL;
    ccb* ccb = FileObject ? FileObject->FsContext2 : NULL;

    FsRtlEnterFileSystem();

    top_level = is_top_level(Irp);
    
    if (Vcb && Vcb->type == VCB_TYPE_PARTITION0) {
        Status = part0_passthrough(DeviceObject, Irp);
        goto exit;
    }
    
    if (!fcb) {
        ERR("fcb was NULL\n");
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }
    
    if (!ccb) {
        ERR("ccb was NULL\n");
        Status = STATUS_INVALID_PARAMETER;
        goto end;
    }
    
    if (fcb->subvol->root_item.flags & BTRFS_SUBVOL_READONLY) {
        Status = STATUS_ACCESS_DENIED;
        goto end;
    }
    
    if (Vcb->readonly) {
        Status = STATUS_MEDIA_WRITE_PROTECTED;
        goto end;
    }
    
    if (Irp->RequestorMode == UserMode && !(ccb->access & (FILE_WRITE_DATA | FILE_APPEND_DATA))) {
        WARN("insufficient permissions\n");
        Status = STATUS_ACCESS_DENIED;
        goto end;
    }
    
//     ERR("recursive = %s\n", Irp != IoGetTopLevelIrp() ? "TRUE" : "FALSE");
    
    try {
        if (IrpSp->MinorFunction & IRP_MN_COMPLETE) {
            CcMdlWriteComplete(IrpSp->FileObject, &IrpSp->Parameters.Write.ByteOffset, Irp->MdlAddress);
            
            Irp->MdlAddress = NULL;
            Status = STATUS_SUCCESS;
        } else {
            Status = write_file(Vcb, Irp, IoIsOperationSynchronous(Irp), FALSE);
        }
    } except (EXCEPTION_EXECUTE_HANDLER) {
        Status = GetExceptionCode();
    }
    
end:
    Irp->IoStatus.Status = Status;

    TRACE("wrote %u bytes\n", Irp->IoStatus.Information);
    
    if (Status != STATUS_PENDING)
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    else {
        IoMarkIrpPending(Irp);
        
        if (!add_thread_job(Vcb, Irp))
            do_write_job(Vcb, Irp);
    }
    
exit:
    if (top_level) 
        IoSetTopLevelIrp(NULL);
    
    FsRtlExitFileSystem();
    
    TRACE("returning %08x\n", Status);

    return Status;
}

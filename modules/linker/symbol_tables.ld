/* Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 */

/* Explicitly place symbol and string tables to avoid warnings */
.symtab 0 (INFO) : { *(.symtab) }
.strtab 0 (INFO) : { *(.strtab) }
.shstrtab 0 (INFO) : { *(.shstrtab) }

// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/HW/GCMemcard/GCMemcard.h"

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstring>
#include <vector>

#include "Common/BitUtils.h"
#include "Common/ColorUtil.h"
#include "Common/CommonFuncs.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/File.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"

static void ByteSwap(u8* valueA, u8* valueB)
{
  u8 tmp = *valueA;
  *valueA = *valueB;
  *valueB = tmp;
}

static constexpr std::optional<u64> BytesToMegabits(u64 bytes)
{
  const u64 factor = ((1024 * 1024) / 8);
  const u64 megabits = bytes / factor;
  const u64 remainder = bytes % factor;
  if (remainder != 0)
    return std::nullopt;
  return megabits;
}

bool GCMemcardErrorCode::HasCriticalErrors() const
{
  return Test(GCMemcardValidityIssues::FAILED_TO_OPEN) || Test(GCMemcardValidityIssues::IO_ERROR) ||
         Test(GCMemcardValidityIssues::INVALID_CARD_SIZE) ||
         Test(GCMemcardValidityIssues::INVALID_CHECKSUM) ||
         Test(GCMemcardValidityIssues::MISMATCHED_CARD_SIZE) ||
         Test(GCMemcardValidityIssues::FREE_BLOCK_MISMATCH);
}

bool GCMemcardErrorCode::Test(GCMemcardValidityIssues code) const
{
  return m_errors.test(static_cast<size_t>(code));
}

void GCMemcardErrorCode::Set(GCMemcardValidityIssues code)
{
  m_errors.set(static_cast<size_t>(code));
}

GCMemcardErrorCode& GCMemcardErrorCode::operator|=(const GCMemcardErrorCode& other)
{
  this->m_errors |= other.m_errors;
  return *this;
}

GCMemcard::GCMemcard()
    : m_valid(false), m_size_blocks(0), m_size_mb(0), m_active_directory(0), m_active_bat(0)
{
}

std::optional<GCMemcard> GCMemcard::Create(std::string filename, u16 size_mbits, bool shift_jis)
{
  GCMemcard card;
  card.m_filename = std::move(filename);

  // TODO: Format() not only formats the card but also writes it to disk at m_filename.
  // Those tasks should probably be separated.
  if (!card.Format(shift_jis, size_mbits))
    return std::nullopt;

  return std::move(card);
}

std::pair<GCMemcardErrorCode, std::optional<GCMemcard>> GCMemcard::Open(std::string filename)
{
  GCMemcardErrorCode error_code;
  File::IOFile file(filename, "rb");
  if (!file.IsOpen())
  {
    error_code.Set(GCMemcardValidityIssues::FAILED_TO_OPEN);
    return std::make_pair(error_code, std::nullopt);
  }

  // check if the filesize is a valid memory card size
  const u64 filesize = file.GetSize();
  const u64 filesize_megabits = BytesToMegabits(filesize).value_or(0);
  const std::array<u16, 6> valid_megabits = {{
      MBIT_SIZE_MEMORY_CARD_59,
      MBIT_SIZE_MEMORY_CARD_123,
      MBIT_SIZE_MEMORY_CARD_251,
      MBIT_SIZE_MEMORY_CARD_507,
      MBIT_SIZE_MEMORY_CARD_1019,
      MBIT_SIZE_MEMORY_CARD_2043,
  }};

  if (!std::any_of(valid_megabits.begin(), valid_megabits.end(),
                   [filesize_megabits](u64 mbits) { return mbits == filesize_megabits; }))
  {
    error_code.Set(GCMemcardValidityIssues::INVALID_CARD_SIZE);
    return std::make_pair(error_code, std::nullopt);
  }

  const u16 card_size_mbits = static_cast<u16>(filesize_megabits);

  // read the entire card into memory
  GCMemcard card;
  file.Seek(0, SEEK_SET);
  if (!file.ReadBytes(&card.m_header_block, BLOCK_SIZE) ||
      !file.ReadBytes(&card.m_directory_blocks[0], BLOCK_SIZE) ||
      !file.ReadBytes(&card.m_directory_blocks[1], BLOCK_SIZE) ||
      !file.ReadBytes(&card.m_bat_blocks[0], BLOCK_SIZE) ||
      !file.ReadBytes(&card.m_bat_blocks[1], BLOCK_SIZE))
  {
    error_code.Set(GCMemcardValidityIssues::IO_ERROR);
    return std::make_pair(error_code, std::nullopt);
  }

  const u16 card_size_blocks = card_size_mbits * MBIT_TO_BLOCKS;
  const u16 user_data_blocks = card_size_blocks - MC_FST_BLOCKS;
  card.m_data_blocks.reserve(user_data_blocks);
  for (u16 i = 0; i < user_data_blocks; ++i)
  {
    GCMBlock& block = card.m_data_blocks.emplace_back();
    if (!file.ReadArray(block.m_block.data(), BLOCK_SIZE))
    {
      error_code.Set(GCMemcardValidityIssues::IO_ERROR);
      return std::make_pair(error_code, std::nullopt);
    }
  }

  file.Close();

  card.m_filename = std::move(filename);
  card.m_size_blocks = card_size_blocks;
  card.m_size_mb = card_size_mbits;

  // can return invalid card size, invalid checksum, data in unused area
  // data in unused area is okay, otherwise fail
  const GCMemcardErrorCode header_error_code = card.m_header_block.CheckForErrors(card_size_mbits);
  error_code |= header_error_code;
  if (header_error_code.HasCriticalErrors())
    return std::make_pair(error_code, std::nullopt);

  // The GC BIOS counts any card as corrupted as long as at least any two of [dir0, dir1, bat0,
  // bat1] are corrupted. Yes, even if we have one valid dir and one valid bat, and even if those
  // are both supposedly the newer ones.
  //
  // If both blocks of a single category are non-corrupted the used block depends on the update
  // counter. If both blocks have the same update counter, it prefers block 0. Otherwise it prefers
  // whichever block has the higher value. Essentially, if (0.update_ctr >= 1.update_ctr) { use 0 }
  // else { use 1 }.
  //
  // If a single block of the four is corrupted, the non-corrupted one of the same category is
  // immediately copied over the corrupted block with an incremented update counter. At this point
  // both blocks contain the same data, so it's hard to tell which one is used, but presumably it
  // uses the one with the now-higher update counter, same as it would have otherwise.
  //
  // This rule only applies for errors within a single block! That is, invalid checksums for both
  // types, and free block mismatch for the BATs. Once two valid blocks have been selected but it
  // later turns out they do not match eachother (eg. claimed block count of a file in the directory
  // does not match the actual block count arrived at by following BAT), the card will be treated as
  // corrupted, even if perhaps a different combination of the two blocks would result in a valid
  // memory card.

  // can return invalid checksum, data in unused area
  GCMemcardErrorCode dir_block_0_error_code = card.m_directory_blocks[0].CheckForErrors();
  GCMemcardErrorCode dir_block_1_error_code = card.m_directory_blocks[1].CheckForErrors();

  // can return invalid card size, invalid checksum, data in unused area, free block mismatch
  GCMemcardErrorCode bat_block_0_error_code = card.m_bat_blocks[0].CheckForErrors(card_size_mbits);
  GCMemcardErrorCode bat_block_1_error_code = card.m_bat_blocks[1].CheckForErrors(card_size_mbits);

  const bool dir_block_0_valid = !dir_block_0_error_code.HasCriticalErrors();
  const bool dir_block_1_valid = !dir_block_1_error_code.HasCriticalErrors();
  const bool bat_block_0_valid = !bat_block_0_error_code.HasCriticalErrors();
  const bool bat_block_1_valid = !bat_block_1_error_code.HasCriticalErrors();

  // if any two (at least) blocks are corrupted return failure
  // TODO: Consider allowing a recovery option when there's still a valid one of each type.
  int number_of_corrupted_dir_bat_blocks = 0;
  if (!dir_block_0_valid)
    ++number_of_corrupted_dir_bat_blocks;
  if (!dir_block_1_valid)
    ++number_of_corrupted_dir_bat_blocks;
  if (!bat_block_0_valid)
    ++number_of_corrupted_dir_bat_blocks;
  if (!bat_block_1_valid)
    ++number_of_corrupted_dir_bat_blocks;

  if (number_of_corrupted_dir_bat_blocks > 1)
  {
    error_code |= dir_block_0_error_code;
    error_code |= dir_block_1_error_code;
    error_code |= bat_block_0_error_code;
    error_code |= bat_block_1_error_code;
    return std::make_pair(error_code, std::nullopt);
  }

  // if exactly one block is corrupted copy and update it over the non-corrupted block
  if (number_of_corrupted_dir_bat_blocks == 1)
  {
    if (!dir_block_0_valid)
    {
      card.m_directory_blocks[0] = card.m_directory_blocks[1];
      card.m_directory_blocks[0].m_update_counter = card.m_directory_blocks[0].m_update_counter + 1;
      card.m_directory_blocks[0].FixChecksums();
      dir_block_0_error_code = card.m_directory_blocks[0].CheckForErrors();
    }
    else if (!dir_block_1_valid)
    {
      card.m_directory_blocks[1] = card.m_directory_blocks[0];
      card.m_directory_blocks[1].m_update_counter = card.m_directory_blocks[1].m_update_counter + 1;
      card.m_directory_blocks[1].FixChecksums();
      dir_block_1_error_code = card.m_directory_blocks[1].CheckForErrors();
    }
    else if (!bat_block_0_valid)
    {
      card.m_bat_blocks[0] = card.m_bat_blocks[1];
      card.m_bat_blocks[0].m_update_counter = card.m_bat_blocks[0].m_update_counter + 1;
      card.m_bat_blocks[0].FixChecksums();
      bat_block_0_error_code = card.m_bat_blocks[0].CheckForErrors(card_size_mbits);
    }
    else if (!bat_block_1_valid)
    {
      card.m_bat_blocks[1] = card.m_bat_blocks[0];
      card.m_bat_blocks[1].m_update_counter = card.m_bat_blocks[1].m_update_counter + 1;
      card.m_bat_blocks[1].FixChecksums();
      bat_block_1_error_code = card.m_bat_blocks[1].CheckForErrors(card_size_mbits);
    }
    else
    {
      // should never reach here
      assert(0);
    }
  }

  error_code |= dir_block_0_error_code;
  error_code |= dir_block_1_error_code;
  error_code |= bat_block_0_error_code;
  error_code |= bat_block_1_error_code;

  // select the in-use Dir and BAT blocks based on update counter
  // TODO: Is there a special case for wraparound after 65535 block updates, or is it assumed that
  // the hardware fails long before that anyway?

  if (card.m_directory_blocks[0].m_update_counter >= card.m_directory_blocks[1].m_update_counter)
    card.m_active_directory = 0;
  else
    card.m_active_directory = 1;

  if (card.m_bat_blocks[0].m_update_counter >= card.m_bat_blocks[1].m_update_counter)
    card.m_active_bat = 0;
  else
    card.m_active_bat = 1;

  // TODO: Do a card-wide corruption check here, such as cross-checking the active Dir's files with
  // the active BAT's allocated sectors. The GC BIOS does at the very least recognize if a file's
  // number of used blocks is inconsistent between the two, and will count a card as corrupted if
  // that is the case.

  card.m_valid = true;

  return std::make_pair(error_code, std::move(card));
}

const Directory& GCMemcard::GetActiveDirectory() const
{
  return m_directory_blocks[m_active_directory];
}

const BlockAlloc& GCMemcard::GetActiveBat() const
{
  return m_bat_blocks[m_active_bat];
}

void GCMemcard::UpdateDirectory(const Directory& directory)
{
  // overwrite inactive dir with given data, then set active dir to written block
  int new_directory_index = m_active_directory == 0 ? 1 : 0;
  m_directory_blocks[new_directory_index] = directory;
  m_active_directory = new_directory_index;
}

void GCMemcard::UpdateBat(const BlockAlloc& bat)
{
  // overwrite inactive BAT with given data, then set active BAT to written block
  int new_bat_index = m_active_bat == 0 ? 1 : 0;
  m_bat_blocks[new_bat_index] = bat;
  m_active_bat = new_bat_index;
}

bool GCMemcard::IsShiftJIS() const
{
  return m_header_block.m_encoding != 0;
}

bool GCMemcard::Save()
{
  File::IOFile mcdFile(m_filename, "wb");
  mcdFile.Seek(0, SEEK_SET);

  mcdFile.WriteBytes(&m_header_block, BLOCK_SIZE);
  mcdFile.WriteBytes(&m_directory_blocks[0], BLOCK_SIZE);
  mcdFile.WriteBytes(&m_directory_blocks[1], BLOCK_SIZE);
  mcdFile.WriteBytes(&m_bat_blocks[0], BLOCK_SIZE);
  mcdFile.WriteBytes(&m_bat_blocks[1], BLOCK_SIZE);
  for (unsigned int i = 0; i < m_size_blocks - MC_FST_BLOCKS; ++i)
  {
    mcdFile.WriteBytes(m_data_blocks[i].m_block.data(), m_data_blocks[i].m_block.size());
  }

  return mcdFile.Close();
}

std::pair<u16, u16> CalculateMemcardChecksums(const u8* data, size_t size)
{
  assert(size % 2 == 0);
  u16 csum = 0;
  u16 inv_csum = 0;

  for (size_t i = 0; i < size; i += 2)
  {
    u16 d = Common::swap16(&data[i]);
    csum += d;
    inv_csum += static_cast<u16>(d ^ 0xffff);
  }

  csum = Common::swap16(csum);
  inv_csum = Common::swap16(inv_csum);

  if (csum == 0xffff)
    csum = 0;
  if (inv_csum == 0xffff)
    inv_csum = 0;

  return std::make_pair(csum, inv_csum);
}

bool GCMemcard::FixChecksums()
{
  if (!m_valid)
    return false;

  m_header_block.FixChecksums();
  m_directory_blocks[0].FixChecksums();
  m_directory_blocks[1].FixChecksums();
  m_bat_blocks[0].FixChecksums();
  m_bat_blocks[1].FixChecksums();

  return true;
}

u8 GCMemcard::GetNumFiles() const
{
  if (!m_valid)
    return 0;

  u8 j = 0;
  for (int i = 0; i < DIRLEN; i++)
  {
    if (GetActiveDirectory().m_dir_entries[i].m_gamecode != DEntry::UNINITIALIZED_GAMECODE)
      j++;
  }
  return j;
}

u8 GCMemcard::GetFileIndex(u8 fileNumber) const
{
  if (m_valid)
  {
    u8 j = 0;
    for (u8 i = 0; i < DIRLEN; i++)
    {
      if (GetActiveDirectory().m_dir_entries[i].m_gamecode != DEntry::UNINITIALIZED_GAMECODE)
      {
        if (j == fileNumber)
        {
          return i;
        }
        j++;
      }
    }
  }
  return 0xFF;
}

u16 GCMemcard::GetFreeBlocks() const
{
  if (!m_valid)
    return 0;

  return GetActiveBat().m_free_blocks;
}

u8 GCMemcard::TitlePresent(const DEntry& d) const
{
  if (!m_valid)
    return DIRLEN;

  u8 i = 0;
  while (i < DIRLEN)
  {
    if (GetActiveDirectory().m_dir_entries[i].m_gamecode == d.m_gamecode &&
        GetActiveDirectory().m_dir_entries[i].m_filename == d.m_filename)
    {
      break;
    }
    i++;
  }
  return i;
}

bool GCMemcard::GCI_FileName(u8 index, std::string& filename) const
{
  if (!m_valid || index >= DIRLEN ||
      GetActiveDirectory().m_dir_entries[index].m_gamecode == DEntry::UNINITIALIZED_GAMECODE)
    return false;

  filename = GetActiveDirectory().m_dir_entries[index].GCI_FileName();
  return true;
}

// DEntry functions, all take u8 index < DIRLEN (127)
// Functions that have ascii output take a char *buffer

std::string GCMemcard::DEntry_GameCode(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return "";

  return std::string(
      reinterpret_cast<const char*>(GetActiveDirectory().m_dir_entries[index].m_gamecode.data()),
      GetActiveDirectory().m_dir_entries[index].m_gamecode.size());
}

std::string GCMemcard::DEntry_Makercode(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return "";

  return std::string(
      reinterpret_cast<const char*>(GetActiveDirectory().m_dir_entries[index].m_makercode.data()),
      GetActiveDirectory().m_dir_entries[index].m_makercode.size());
}

std::string GCMemcard::DEntry_BIFlags(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return "";

  std::string flags;
  int x = GetActiveDirectory().m_dir_entries[index].m_banner_and_icon_flags;
  for (int i = 0; i < 8; i++)
  {
    flags.push_back((x & 0x80) ? '1' : '0');
    x = x << 1;
  }
  return flags;
}

std::string GCMemcard::DEntry_FileName(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return "";

  return std::string(
      reinterpret_cast<const char*>(GetActiveDirectory().m_dir_entries[index].m_filename.data()),
      GetActiveDirectory().m_dir_entries[index].m_filename.size());
}

u32 GCMemcard::DEntry_ModTime(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return 0xFFFFFFFF;

  return GetActiveDirectory().m_dir_entries[index].m_modification_time;
}

u32 GCMemcard::DEntry_ImageOffset(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return 0xFFFFFFFF;

  return GetActiveDirectory().m_dir_entries[index].m_image_offset;
}

std::string GCMemcard::DEntry_IconFmt(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return "";

  u16 x = GetActiveDirectory().m_dir_entries[index].m_icon_format;
  std::string format;
  for (size_t i = 0; i < 16; ++i)
  {
    format.push_back(Common::ExtractBit(x, 15 - i) ? '1' : '0');
  }
  return format;
}

std::string GCMemcard::DEntry_AnimSpeed(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return "";

  u16 x = GetActiveDirectory().m_dir_entries[index].m_animation_speed;
  std::string speed;
  for (size_t i = 0; i < 16; ++i)
  {
    speed.push_back(Common::ExtractBit(x, 15 - i) ? '1' : '0');
  }
  return speed;
}

std::string GCMemcard::DEntry_Permissions(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return "";

  u8 Permissions = GetActiveDirectory().m_dir_entries[index].m_file_permissions;
  std::string permissionsString;
  permissionsString.push_back((Permissions & 16) ? 'x' : 'M');
  permissionsString.push_back((Permissions & 8) ? 'x' : 'C');
  permissionsString.push_back((Permissions & 4) ? 'P' : 'x');
  return permissionsString;
}

u8 GCMemcard::DEntry_CopyCounter(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return 0xFF;

  return GetActiveDirectory().m_dir_entries[index].m_copy_counter;
}

u16 GCMemcard::DEntry_FirstBlock(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return 0xFFFF;

  u16 block = GetActiveDirectory().m_dir_entries[index].m_first_block;
  if (block > (u16)m_size_blocks)
    return 0xFFFF;
  return block;
}

u16 GCMemcard::DEntry_BlockCount(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return 0xFFFF;

  u16 blocks = GetActiveDirectory().m_dir_entries[index].m_block_count;
  if (blocks > (u16)m_size_blocks)
    return 0xFFFF;
  return blocks;
}

u32 GCMemcard::DEntry_CommentsAddress(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return 0xFFFF;

  return GetActiveDirectory().m_dir_entries[index].m_comments_address;
}

std::string GCMemcard::GetSaveComment1(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return "";

  u32 Comment1 = GetActiveDirectory().m_dir_entries[index].m_comments_address;
  u32 DataBlock = GetActiveDirectory().m_dir_entries[index].m_first_block - MC_FST_BLOCKS;
  if ((DataBlock > m_size_blocks) || (Comment1 == 0xFFFFFFFF))
  {
    return "";
  }
  return std::string((const char*)m_data_blocks[DataBlock].m_block.data() + Comment1,
                     DENTRY_STRLEN);
}

std::string GCMemcard::GetSaveComment2(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return "";

  u32 Comment1 = GetActiveDirectory().m_dir_entries[index].m_comments_address;
  u32 Comment2 = Comment1 + DENTRY_STRLEN;
  u32 DataBlock = GetActiveDirectory().m_dir_entries[index].m_first_block - MC_FST_BLOCKS;
  if ((DataBlock > m_size_blocks) || (Comment1 == 0xFFFFFFFF))
  {
    return "";
  }
  return std::string((const char*)m_data_blocks[DataBlock].m_block.data() + Comment2,
                     DENTRY_STRLEN);
}

std::optional<DEntry> GCMemcard::GetDEntry(u8 index) const
{
  if (!m_valid || index >= DIRLEN)
    return std::nullopt;

  return GetActiveDirectory().m_dir_entries[index];
}

BlockAlloc::BlockAlloc(u16 size_mbits)
{
  memset(this, 0, BLOCK_SIZE);
  m_free_blocks = (size_mbits * MBIT_TO_BLOCKS) - MC_FST_BLOCKS;
  m_last_allocated_block = 4;
  FixChecksums();
}

u16 BlockAlloc::GetNextBlock(u16 block) const
{
  // FIXME: This is fishy, shouldn't that be in range [5, 4096[?
  if ((block < MC_FST_BLOCKS) || (block > 4091))
    return 0;

  return m_map[block - MC_FST_BLOCKS];
}

// Parameters and return value are expected as memory card block index,
// not BAT index; that is, block 5 is the first file data block.
u16 BlockAlloc::NextFreeBlock(u16 max_block, u16 starting_block) const
{
  if (m_free_blocks > 0)
  {
    starting_block = std::clamp<u16>(starting_block, MC_FST_BLOCKS, BAT_SIZE + MC_FST_BLOCKS);
    max_block = std::clamp<u16>(max_block, MC_FST_BLOCKS, BAT_SIZE + MC_FST_BLOCKS);
    for (u16 i = starting_block; i < max_block; ++i)
      if (m_map[i - MC_FST_BLOCKS] == 0)
        return i;

    for (u16 i = MC_FST_BLOCKS; i < starting_block; ++i)
      if (m_map[i - MC_FST_BLOCKS] == 0)
        return i;
  }
  return 0xFFFF;
}

bool BlockAlloc::ClearBlocks(u16 starting_block, u16 block_count)
{
  std::vector<u16> blocks;
  while (starting_block != 0xFFFF && starting_block != 0)
  {
    blocks.push_back(starting_block);
    starting_block = GetNextBlock(starting_block);
  }
  if (starting_block > 0)
  {
    size_t length = blocks.size();
    if (length != block_count)
    {
      return false;
    }
    for (unsigned int i = 0; i < length; ++i)
      m_map[blocks.at(i) - MC_FST_BLOCKS] = 0;
    m_free_blocks = m_free_blocks + block_count;

    return true;
  }
  return false;
}

void BlockAlloc::FixChecksums()
{
  std::tie(m_checksum, m_checksum_inv) = CalculateChecksums();
}

u16 BlockAlloc::AssignBlocksContiguous(u16 length)
{
  u16 starting = m_last_allocated_block + 1;
  if (length > m_free_blocks)
    return 0xFFFF;
  u16 current = starting;
  while ((current - starting + 1) < length)
  {
    m_map[current - 5] = current + 1;
    current++;
  }
  m_map[current - 5] = 0xFFFF;
  m_last_allocated_block = current;
  m_free_blocks = m_free_blocks - length;
  FixChecksums();
  return starting;
}

std::pair<u16, u16> BlockAlloc::CalculateChecksums() const
{
  static_assert(std::is_trivially_copyable<BlockAlloc>());

  std::array<u8, sizeof(BlockAlloc)> raw;
  memcpy(raw.data(), this, raw.size());

  constexpr size_t checksum_area_start = offsetof(BlockAlloc, m_update_counter);
  constexpr size_t checksum_area_end = sizeof(BlockAlloc);
  constexpr size_t checksum_area_size = checksum_area_end - checksum_area_start;
  return CalculateMemcardChecksums(&raw[checksum_area_start], checksum_area_size);
}

GCMemcardErrorCode BlockAlloc::CheckForErrors(u16 size_mbits) const
{
  GCMemcardErrorCode error_code;

  // verify checksums
  const auto [checksum_sum, checksum_inv] = CalculateChecksums();
  if (checksum_sum != m_checksum || checksum_inv != m_checksum_inv)
    error_code.Set(GCMemcardValidityIssues::INVALID_CHECKSUM);

  if (size_mbits > 0 && size_mbits <= 256)
  {
    // check if free block count matches the actual amount of free blocks in m_map
    const u16 total_available_blocks = (size_mbits * MBIT_TO_BLOCKS) - MC_FST_BLOCKS;
    assert(total_available_blocks <= m_map.size());
    u16 blocks_in_use = 0;
    for (size_t i = 0; i < total_available_blocks; ++i)
    {
      if (m_map[i] != 0)
        ++blocks_in_use;
    }
    const u16 free_blocks = total_available_blocks - blocks_in_use;

    if (free_blocks != m_free_blocks)
      error_code.Set(GCMemcardValidityIssues::FREE_BLOCK_MISMATCH);

    // remaining blocks map to nothing on hardware and must be empty
    for (size_t i = total_available_blocks; i < m_map.size(); ++i)
    {
      if (m_map[i] != 0)
      {
        error_code.Set(GCMemcardValidityIssues::DATA_IN_UNUSED_AREA);
        break;
      }
    }
  }
  else
  {
    // card size is outside the range of blocks that can be addressed
    error_code.Set(GCMemcardValidityIssues::INVALID_CARD_SIZE);
  }

  return error_code;
}

GCMemcardGetSaveDataRetVal GCMemcard::GetSaveData(u8 index, std::vector<GCMBlock>& Blocks) const
{
  if (!m_valid)
    return GCMemcardGetSaveDataRetVal::NOMEMCARD;

  u16 block = DEntry_FirstBlock(index);
  u16 BlockCount = DEntry_BlockCount(index);
  // u16 memcardSize = BE16(hdr.m_size_mb) * MBIT_TO_BLOCKS;

  if ((block == 0xFFFF) || (BlockCount == 0xFFFF))
  {
    return GCMemcardGetSaveDataRetVal::FAIL;
  }

  u16 nextBlock = block;
  for (int i = 0; i < BlockCount; ++i)
  {
    if ((!nextBlock) || (nextBlock == 0xFFFF))
      return GCMemcardGetSaveDataRetVal::FAIL;
    Blocks.push_back(m_data_blocks[nextBlock - MC_FST_BLOCKS]);
    nextBlock = GetActiveBat().GetNextBlock(nextBlock);
  }
  return GCMemcardGetSaveDataRetVal::SUCCESS;
}
// End DEntry functions

GCMemcardImportFileRetVal GCMemcard::ImportFile(const DEntry& direntry,
                                                std::vector<GCMBlock>& saveBlocks)
{
  if (!m_valid)
    return GCMemcardImportFileRetVal::NOMEMCARD;

  if (GetNumFiles() >= DIRLEN)
  {
    return GCMemcardImportFileRetVal::OUTOFDIRENTRIES;
  }
  if (GetActiveBat().m_free_blocks < direntry.m_block_count)
  {
    return GCMemcardImportFileRetVal::OUTOFBLOCKS;
  }
  if (TitlePresent(direntry) != DIRLEN)
  {
    return GCMemcardImportFileRetVal::TITLEPRESENT;
  }

  // find first free data block
  u16 firstBlock =
      GetActiveBat().NextFreeBlock(m_size_blocks, GetActiveBat().m_last_allocated_block);
  if (firstBlock == 0xFFFF)
    return GCMemcardImportFileRetVal::OUTOFBLOCKS;
  Directory UpdatedDir = GetActiveDirectory();

  // find first free dir entry
  for (int i = 0; i < DIRLEN; i++)
  {
    if (UpdatedDir.m_dir_entries[i].m_gamecode == DEntry::UNINITIALIZED_GAMECODE)
    {
      UpdatedDir.m_dir_entries[i] = direntry;
      UpdatedDir.m_dir_entries[i].m_first_block = firstBlock;
      UpdatedDir.m_dir_entries[i].m_copy_counter = UpdatedDir.m_dir_entries[i].m_copy_counter + 1;
      break;
    }
  }
  UpdatedDir.m_update_counter = UpdatedDir.m_update_counter + 1;
  UpdateDirectory(UpdatedDir);

  int fileBlocks = direntry.m_block_count;

  FZEROGX_MakeSaveGameValid(m_header_block, direntry, saveBlocks);
  PSO_MakeSaveGameValid(m_header_block, direntry, saveBlocks);

  BlockAlloc UpdatedBat = GetActiveBat();
  u16 nextBlock;
  // keep assuming no freespace fragmentation, and copy over all the data
  for (int i = 0; i < fileBlocks; ++i)
  {
    if (firstBlock == 0xFFFF)
      PanicAlert("Fatal Error");
    m_data_blocks[firstBlock - MC_FST_BLOCKS] = saveBlocks[i];
    if (i == fileBlocks - 1)
      nextBlock = 0xFFFF;
    else
      nextBlock = UpdatedBat.NextFreeBlock(m_size_blocks, firstBlock + 1);
    UpdatedBat.m_map[firstBlock - MC_FST_BLOCKS] = nextBlock;
    UpdatedBat.m_last_allocated_block = firstBlock;
    firstBlock = nextBlock;
  }

  UpdatedBat.m_free_blocks = UpdatedBat.m_free_blocks - fileBlocks;
  UpdatedBat.m_update_counter = UpdatedBat.m_update_counter + 1;
  UpdateBat(UpdatedBat);

  FixChecksums();

  return GCMemcardImportFileRetVal::SUCCESS;
}

GCMemcardRemoveFileRetVal GCMemcard::RemoveFile(u8 index)  // index in the directory array
{
  if (!m_valid)
    return GCMemcardRemoveFileRetVal::NOMEMCARD;
  if (index >= DIRLEN)
    return GCMemcardRemoveFileRetVal::DELETE_FAIL;

  u16 startingblock = GetActiveDirectory().m_dir_entries[index].m_first_block;
  u16 numberofblocks = GetActiveDirectory().m_dir_entries[index].m_block_count;

  BlockAlloc UpdatedBat = GetActiveBat();
  if (!UpdatedBat.ClearBlocks(startingblock, numberofblocks))
    return GCMemcardRemoveFileRetVal::DELETE_FAIL;
  UpdatedBat.m_update_counter = UpdatedBat.m_update_counter + 1;
  UpdateBat(UpdatedBat);

  Directory UpdatedDir = GetActiveDirectory();

  // TODO: Deleting a file via the GC BIOS sometimes leaves behind an extra updated directory block
  // here that has an empty file with the filename "Broken File000" where the actual deleted file
  // was. Determine when exactly this happens and if this is neccessary for anything.

  memset(&(UpdatedDir.m_dir_entries[index]), 0xFF, DENTRY_SIZE);
  UpdatedDir.m_update_counter = UpdatedDir.m_update_counter + 1;
  UpdateDirectory(UpdatedDir);

  FixChecksums();

  return GCMemcardRemoveFileRetVal::SUCCESS;
}

GCMemcardImportFileRetVal GCMemcard::CopyFrom(const GCMemcard& source, u8 index)
{
  if (!m_valid || !source.m_valid)
    return GCMemcardImportFileRetVal::NOMEMCARD;

  std::optional<DEntry> tempDEntry = source.GetDEntry(index);
  if (!tempDEntry)
    return GCMemcardImportFileRetVal::NOMEMCARD;

  u32 size = source.DEntry_BlockCount(index);
  if (size == 0xFFFF)
    return GCMemcardImportFileRetVal::INVALIDFILESIZE;

  std::vector<GCMBlock> saveData;
  saveData.reserve(size);
  switch (source.GetSaveData(index, saveData))
  {
  case GCMemcardGetSaveDataRetVal::FAIL:
    return GCMemcardImportFileRetVal::FAIL;
  case GCMemcardGetSaveDataRetVal::NOMEMCARD:
    return GCMemcardImportFileRetVal::NOMEMCARD;
  default:
    FixChecksums();
    return ImportFile(*tempDEntry, saveData);
  }
}

GCMemcardImportFileRetVal GCMemcard::ImportGci(const std::string& inputFile)
{
  if (!m_valid)
    return GCMemcardImportFileRetVal::OPENFAIL;

  File::IOFile gci(inputFile, "rb");
  if (!gci)
    return GCMemcardImportFileRetVal::OPENFAIL;

  return ImportGciInternal(std::move(gci), inputFile);
}

GCMemcardImportFileRetVal GCMemcard::ImportGciInternal(File::IOFile&& gci,
                                                       const std::string& inputFile)
{
  unsigned int offset;
  std::string fileType;
  SplitPath(inputFile, nullptr, nullptr, &fileType);

  if (!strcasecmp(fileType.c_str(), ".gci"))
    offset = GCI;
  else
  {
    char tmp[0xD];
    gci.ReadBytes(tmp, sizeof(tmp));
    if (!strcasecmp(fileType.c_str(), ".gcs"))
    {
      if (!memcmp(tmp, "GCSAVE", 6))  // Header must be uppercase
        offset = GCS;
      else
        return GCMemcardImportFileRetVal::GCSFAIL;
    }
    else if (!strcasecmp(fileType.c_str(), ".sav"))
    {
      if (!memcmp(tmp, "DATELGC_SAVE", 0xC))  // Header must be uppercase
        offset = SAV;
      else
        return GCMemcardImportFileRetVal::SAVFAIL;
    }
    else
      return GCMemcardImportFileRetVal::OPENFAIL;
  }
  gci.Seek(offset, SEEK_SET);

  DEntry tempDEntry;
  gci.ReadBytes(&tempDEntry, DENTRY_SIZE);
  const u64 fStart = gci.Tell();
  gci.Seek(0, SEEK_END);
  const u64 length = gci.Tell() - fStart;
  gci.Seek(offset + DENTRY_SIZE, SEEK_SET);

  Gcs_SavConvert(tempDEntry, offset, length);

  if (length != tempDEntry.m_block_count * BLOCK_SIZE)
    return GCMemcardImportFileRetVal::LENGTHFAIL;
  if (gci.Tell() != offset + DENTRY_SIZE)  // Verify correct file position
    return GCMemcardImportFileRetVal::OPENFAIL;

  u32 size = tempDEntry.m_block_count;
  std::vector<GCMBlock> saveData;
  saveData.reserve(size);

  for (unsigned int i = 0; i < size; ++i)
  {
    GCMBlock b;
    gci.ReadBytes(b.m_block.data(), b.m_block.size());
    saveData.push_back(b);
  }
  return ImportFile(tempDEntry, saveData);
}

GCMemcardExportFileRetVal GCMemcard::ExportGci(u8 index, const std::string& fileName,
                                               const std::string& directory) const
{
  File::IOFile gci;
  int offset = GCI;

  if (!fileName.length())
  {
    std::string gciFilename;
    // GCI_FileName should only fail if the gamecode is 0xFFFFFFFF
    if (!GCI_FileName(index, gciFilename))
      return GCMemcardExportFileRetVal::SUCCESS;
    gci.Open(directory + DIR_SEP + gciFilename, "wb");
  }
  else
  {
    std::string fileType;
    gci.Open(fileName, "wb");
    SplitPath(fileName, nullptr, nullptr, &fileType);
    if (!strcasecmp(fileType.c_str(), ".gcs"))
    {
      offset = GCS;
    }
    else if (!strcasecmp(fileType.c_str(), ".sav"))
    {
      offset = SAV;
    }
  }

  if (!gci)
    return GCMemcardExportFileRetVal::OPENFAIL;

  gci.Seek(0, SEEK_SET);

  switch (offset)
  {
  case GCS:
    u8 gcsHDR[GCS];
    memset(gcsHDR, 0, GCS);
    memcpy(gcsHDR, "GCSAVE", 6);
    gci.WriteArray(gcsHDR, GCS);
    break;

  case SAV:
    u8 savHDR[SAV];
    memset(savHDR, 0, SAV);
    memcpy(savHDR, "DATELGC_SAVE", 0xC);
    gci.WriteArray(savHDR, SAV);
    break;
  }

  std::optional<DEntry> tempDEntry = GetDEntry(index);
  if (!tempDEntry)
    return GCMemcardExportFileRetVal::NOMEMCARD;

  Gcs_SavConvert(*tempDEntry, offset);
  gci.WriteBytes(&tempDEntry.value(), DENTRY_SIZE);

  u32 size = DEntry_BlockCount(index);
  if (size == 0xFFFF)
  {
    return GCMemcardExportFileRetVal::FAIL;
  }

  std::vector<GCMBlock> saveData;
  saveData.reserve(size);

  switch (GetSaveData(index, saveData))
  {
  case GCMemcardGetSaveDataRetVal::FAIL:
    return GCMemcardExportFileRetVal::FAIL;
  case GCMemcardGetSaveDataRetVal::NOMEMCARD:
    return GCMemcardExportFileRetVal::NOMEMCARD;
  }
  gci.Seek(DENTRY_SIZE + offset, SEEK_SET);
  for (unsigned int i = 0; i < size; ++i)
  {
    gci.WriteBytes(saveData[i].m_block.data(), saveData[i].m_block.size());
  }

  if (gci.IsGood())
    return GCMemcardExportFileRetVal::SUCCESS;
  else
    return GCMemcardExportFileRetVal::WRITEFAIL;
}

void GCMemcard::Gcs_SavConvert(DEntry& tempDEntry, int saveType, u64 length)
{
  switch (saveType)
  {
  case GCS:
  {
    // field containing the Block count as displayed within
    // the GameSaves software is not stored in the GCS file.
    // It is stored only within the corresponding GSV file.
    // If the GCS file is added without using the GameSaves software,
    // the value stored is always "1"
    tempDEntry.m_block_count = length / BLOCK_SIZE;
  }
  break;
  case SAV:
    // swap byte pairs
    // 0x2C and 0x2D, 0x2E and 0x2F, 0x30 and 0x31, 0x32 and 0x33,
    // 0x34 and 0x35, 0x36 and 0x37, 0x38 and 0x39, 0x3A and 0x3B,
    // 0x3C and 0x3D,0x3E and 0x3F.
    // It seems that sav files also swap the banner/icon flags...
    ByteSwap(&tempDEntry.m_unused_1, &tempDEntry.m_banner_and_icon_flags);

    std::array<u8, 4> tmp;
    memcpy(tmp.data(), &tempDEntry.m_image_offset, 4);
    ByteSwap(&tmp[0], &tmp[1]);
    ByteSwap(&tmp[2], &tmp[3]);
    memcpy(&tempDEntry.m_image_offset, tmp.data(), 4);

    memcpy(tmp.data(), &tempDEntry.m_icon_format, 2);
    ByteSwap(&tmp[0], &tmp[1]);
    memcpy(&tempDEntry.m_icon_format, tmp.data(), 2);

    memcpy(tmp.data(), &tempDEntry.m_animation_speed, 2);
    ByteSwap(&tmp[0], &tmp[1]);
    memcpy(&tempDEntry.m_animation_speed, tmp.data(), 2);

    ByteSwap(&tempDEntry.m_file_permissions, &tempDEntry.m_copy_counter);

    memcpy(tmp.data(), &tempDEntry.m_first_block, 2);
    ByteSwap(&tmp[0], &tmp[1]);
    memcpy(&tempDEntry.m_first_block, tmp.data(), 2);

    memcpy(tmp.data(), &tempDEntry.m_block_count, 2);
    ByteSwap(&tmp[0], &tmp[1]);
    memcpy(&tempDEntry.m_block_count, tmp.data(), 2);

    ByteSwap(&tempDEntry.m_unused_2[0], &tempDEntry.m_unused_2[1]);

    memcpy(tmp.data(), &tempDEntry.m_comments_address, 4);
    ByteSwap(&tmp[0], &tmp[1]);
    ByteSwap(&tmp[2], &tmp[3]);
    memcpy(&tempDEntry.m_comments_address, tmp.data(), 4);
    break;
  default:
    break;
  }
}

bool GCMemcard::ReadBannerRGBA8(u8 index, u32* buffer) const
{
  if (!m_valid || index >= DIRLEN)
    return false;

  int flags = GetActiveDirectory().m_dir_entries[index].m_banner_and_icon_flags;
  // Timesplitters 2 is the only game that I see this in
  // May be a hack
  if (flags == 0xFB)
    flags = ~flags;

  int bnrFormat = (flags & 3);

  if (bnrFormat == 0)
    return false;

  u32 DataOffset = GetActiveDirectory().m_dir_entries[index].m_image_offset;
  u32 DataBlock = GetActiveDirectory().m_dir_entries[index].m_first_block - MC_FST_BLOCKS;

  if ((DataBlock > m_size_blocks) || (DataOffset == 0xFFFFFFFF))
  {
    return false;
  }

  const int pixels = 96 * 32;

  if (bnrFormat & 1)
  {
    u8* pxdata = (u8*)(m_data_blocks[DataBlock].m_block.data() + DataOffset);
    u16* paldata = (u16*)(m_data_blocks[DataBlock].m_block.data() + DataOffset + pixels);

    Common::DecodeCI8Image(buffer, pxdata, paldata, 96, 32);
  }
  else
  {
    u16* pxdata = (u16*)(m_data_blocks[DataBlock].m_block.data() + DataOffset);

    Common::Decode5A3Image(buffer, pxdata, 96, 32);
  }
  return true;
}

u32 GCMemcard::ReadAnimRGBA8(u8 index, u32* buffer, u8* delays) const
{
  if (!m_valid || index >= DIRLEN)
    return 0;

  // To ensure only one type of icon is used
  // Sonic Heroes it the only game I have seen that tries to use a CI8 and RGB5A3 icon
  // int fmtCheck = 0;

  int formats = GetActiveDirectory().m_dir_entries[index].m_icon_format;
  int fdelays = GetActiveDirectory().m_dir_entries[index].m_animation_speed;

  int flags = GetActiveDirectory().m_dir_entries[index].m_banner_and_icon_flags;
  // Timesplitters 2 and 3 is the only game that I see this in
  // May be a hack
  // if (flags == 0xFB) flags = ~flags;
  // Batten Kaitos has 0x65 as flag too. Everything but the first 3 bytes seems irrelevant.
  // Something similar happens with Wario Ware Inc. AnimSpeed

  int bnrFormat = (flags & 3);

  u32 DataOffset = GetActiveDirectory().m_dir_entries[index].m_image_offset;
  u32 DataBlock = GetActiveDirectory().m_dir_entries[index].m_first_block - MC_FST_BLOCKS;

  if ((DataBlock > m_size_blocks) || (DataOffset == 0xFFFFFFFF))
  {
    return 0;
  }

  u8* animData = (u8*)(m_data_blocks[DataBlock].m_block.data() + DataOffset);

  switch (bnrFormat)
  {
  case 1:
    animData += 96 * 32 + 2 * 256;  // image+palette
    break;
  case 2:
    animData += 96 * 32 * 2;
    break;
  }

  int fmts[8];
  u8* data[8];
  int frames = 0;

  for (int i = 0; i < 8; i++)
  {
    fmts[i] = (formats >> (2 * i)) & 3;
    delays[i] = ((fdelays >> (2 * i)) & 3);
    data[i] = animData;

    if (!delays[i])
    {
      // First icon_speed = 0 indicates there aren't any more icons
      break;
    }
    // If speed is set there is an icon (it can be a "blank frame")
    frames++;
    if (fmts[i] != 0)
    {
      switch (fmts[i])
      {
      case CI8SHARED:  // CI8 with shared palette
        animData += 32 * 32;
        break;
      case RGB5A3:  // RGB5A3
        animData += 32 * 32 * 2;
        break;
      case CI8:  // CI8 with own palette
        animData += 32 * 32 + 2 * 256;
        break;
      }
    }
  }

  const u16* sharedPal = reinterpret_cast<u16*>(animData);

  for (int i = 0; i < 8; i++)
  {
    if (!delays[i])
    {
      // First icon_speed = 0 indicates there aren't any more icons
      break;
    }
    if (fmts[i] != 0)
    {
      switch (fmts[i])
      {
      case CI8SHARED:  // CI8 with shared palette
        Common::DecodeCI8Image(buffer, data[i], sharedPal, 32, 32);
        buffer += 32 * 32;
        break;
      case RGB5A3:  // RGB5A3
        Common::Decode5A3Image(buffer, (u16*)(data[i]), 32, 32);
        buffer += 32 * 32;
        break;
      case CI8:  // CI8 with own palette
        const u16* paldata = reinterpret_cast<u16*>(data[i] + 32 * 32);
        Common::DecodeCI8Image(buffer, data[i], paldata, 32, 32);
        buffer += 32 * 32;
        break;
      }
    }
    else
    {
      // Speed is set but there's no actual icon
      // This is used to reduce animation speed in Pikmin and Luigi's Mansion for example
      // These "blank frames" show the next icon
      for (int j = i; j < 8; ++j)
      {
        if (fmts[j] != 0)
        {
          switch (fmts[j])
          {
          case CI8SHARED:  // CI8 with shared palette
            Common::DecodeCI8Image(buffer, data[j], sharedPal, 32, 32);
            break;
          case RGB5A3:  // RGB5A3
            Common::Decode5A3Image(buffer, (u16*)(data[j]), 32, 32);
            buffer += 32 * 32;
            break;
          case CI8:  // CI8 with own palette
            const u16* paldata = reinterpret_cast<u16*>(data[j] + 32 * 32);
            Common::DecodeCI8Image(buffer, data[j], paldata, 32, 32);
            buffer += 32 * 32;
            break;
          }
        }
      }
    }
  }

  return frames;
}

bool GCMemcard::Format(u8* card_data, bool shift_jis, u16 SizeMb)
{
  if (!card_data)
    return false;
  memset(card_data, 0xFF, BLOCK_SIZE * 3);
  memset(card_data + BLOCK_SIZE * 3, 0, BLOCK_SIZE * 2);

  *((Header*)card_data) = Header(SLOT_A, SizeMb, shift_jis);

  *((Directory*)(card_data + BLOCK_SIZE)) = Directory();
  *((Directory*)(card_data + BLOCK_SIZE * 2)) = Directory();
  *((BlockAlloc*)(card_data + BLOCK_SIZE * 3)) = BlockAlloc(SizeMb);
  *((BlockAlloc*)(card_data + BLOCK_SIZE * 4)) = BlockAlloc(SizeMb);
  return true;
}

bool GCMemcard::Format(bool shift_jis, u16 SizeMb)
{
  m_header_block = Header(SLOT_A, SizeMb, shift_jis);
  m_directory_blocks[0] = m_directory_blocks[1] = Directory();
  m_bat_blocks[0] = m_bat_blocks[1] = BlockAlloc(SizeMb);

  m_size_mb = SizeMb;
  m_size_blocks = (u32)m_size_mb * MBIT_TO_BLOCKS;
  m_data_blocks.clear();
  m_data_blocks.resize(m_size_blocks - MC_FST_BLOCKS);

  m_active_directory = 0;
  m_active_bat = 0;
  m_valid = true;

  return Save();
}

/*************************************************************/
/* FZEROGX_MakeSaveGameValid                                 */
/* (use just before writing a F-Zero GX system .gci file)    */
/*                                                           */
/* Parameters:                                               */
/*    direntry:   [Description needed]                       */
/*    FileBuffer: [Description needed]                       */
/*                                                           */
/* Returns: Error code                                       */
/*************************************************************/

s32 GCMemcard::FZEROGX_MakeSaveGameValid(const Header& cardheader, const DEntry& direntry,
                                         std::vector<GCMBlock>& FileBuffer)
{
  u32 i, j;
  u16 chksum = 0xFFFF;

  // check for F-Zero GX system file
  if (strcmp(reinterpret_cast<const char*>(direntry.m_filename.data()), "f_zero.dat") != 0)
    return 0;

  // also make sure that the filesize is correct
  if (FileBuffer.size() != 4)
    return 0;

  // get encrypted destination memory card serial numbers
  const auto [serial1, serial2] = cardheader.CalculateSerial();

  // set new serial numbers
  *(u16*)&FileBuffer[1].m_block[0x0066] = BE16(BE32(serial1) >> 16);
  *(u16*)&FileBuffer[3].m_block[0x1580] = BE16(BE32(serial2) >> 16);
  *(u16*)&FileBuffer[1].m_block[0x0060] = BE16(BE32(serial1) & 0xFFFF);
  *(u16*)&FileBuffer[1].m_block[0x0200] = BE16(BE32(serial2) & 0xFFFF);

  // calc 16-bit checksum
  for (i = 0x02; i < 0x8000; i++)
  {
    const int block = i / 0x2000;
    const int offset = i % 0x2000;
    chksum ^= (FileBuffer[block].m_block[offset] & 0xFF);
    for (j = 8; j > 0; j--)
    {
      if (chksum & 1)
        chksum = (chksum >> 1) ^ 0x8408;
      else
        chksum >>= 1;
    }
  }

  // set new checksum
  *(u16*)&FileBuffer[0].m_block[0x00] = BE16(~chksum);

  return 1;
}

/***********************************************************/
/* PSO_MakeSaveGameValid                                   */
/* (use just before writing a PSO system .gci file)        */
/*                                                         */
/* Parameters:                                             */
/*    direntry:   [Description needed]                     */
/*    FileBuffer: [Description needed]                     */
/*                                                         */
/* Returns: Error code                                     */
/***********************************************************/

s32 GCMemcard::PSO_MakeSaveGameValid(const Header& cardheader, const DEntry& direntry,
                                     std::vector<GCMBlock>& FileBuffer)
{
  u32 i, j;
  u32 chksum;
  u32 crc32LUT[256];
  u32 pso3offset = 0x00;

  // check for PSO1&2 system file
  if (strcmp(reinterpret_cast<const char*>(direntry.m_filename.data()), "PSO_SYSTEM") != 0)
  {
    // check for PSO3 system file
    if (strcmp(reinterpret_cast<const char*>(direntry.m_filename.data()), "PSO3_SYSTEM") == 0)
    {
      // PSO3 data block size adjustment
      pso3offset = 0x10;
    }
    else
    {
      // nothing to do
      return 0;
    }
  }

  // get encrypted destination memory card serial numbers
  const auto [serial1, serial2] = cardheader.CalculateSerial();

  // set new serial numbers
  *(u32*)&FileBuffer[1].m_block[0x0158] = serial1;
  *(u32*)&FileBuffer[1].m_block[0x015C] = serial2;

  // generate crc32 LUT
  for (i = 0; i < 256; i++)
  {
    chksum = i;
    for (j = 8; j > 0; j--)
    {
      if (chksum & 1)
        chksum = (chksum >> 1) ^ 0xEDB88320;
      else
        chksum >>= 1;
    }

    crc32LUT[i] = chksum;
  }

  // PSO initial crc32 value
  chksum = 0xDEBB20E3;

  // calc 32-bit checksum
  for (i = 0x004C; i < 0x0164 + pso3offset; i++)
  {
    chksum = ((chksum >> 8) & 0xFFFFFF) ^ crc32LUT[(chksum ^ FileBuffer[1].m_block[i]) & 0xFF];
  }

  // set new checksum
  *(u32*)&FileBuffer[1].m_block[0x0048] = BE32(chksum ^ 0xFFFFFFFF);

  return 1;
}

GCMBlock::GCMBlock()
{
  Erase();
}

void GCMBlock::Erase()
{
  memset(m_block.data(), 0xFF, m_block.size());
}

Header::Header(int slot, u16 size_mbits, bool shift_jis)
{
  // Nintendo format algorithm.
  // Constants are fixed by the GC SDK
  // Changing the constants will break memory card support
  memset(this, 0xFF, BLOCK_SIZE);
  m_size_mb = size_mbits;
  m_encoding = shift_jis ? 1 : 0;
  u64 rand = Common::Timer::GetLocalTimeSinceJan1970() - ExpansionInterface::CEXIIPL::GC_EPOCH;
  m_format_time = rand;
  for (int i = 0; i < 12; i++)
  {
    rand = (((rand * (u64)0x0000000041c64e6dULL) + (u64)0x0000000000003039ULL) >> 16);
    m_serial[i] = (u8)(g_SRAM.settings_ex.flash_id[slot][i] + (u32)rand);
    rand = (((rand * (u64)0x0000000041c64e6dULL) + (u64)0x0000000000003039ULL) >> 16);
    rand &= (u64)0x0000000000007fffULL;
  }
  m_sram_bias = g_SRAM.settings.rtc_bias;
  m_sram_language = static_cast<u32>(g_SRAM.settings.language);
  // TODO: determine the purpose of m_unknown_2
  // 1 works for slot A, 0 works for both slot A and slot B
  memset(m_unknown_2.data(), 0,
         m_unknown_2.size());  // = _viReg[55];  static vu16* const _viReg = (u16*)0xCC002000;
  m_device_id = 0;
  FixChecksums();
}

std::pair<u32, u32> Header::CalculateSerial() const
{
  static_assert(std::is_trivially_copyable<Header>());

  std::array<u8, 32> raw;
  memcpy(raw.data(), this, raw.size());

  u32 serial1 = 0;
  u32 serial2 = 0;
  for (size_t i = 0; i < raw.size(); i += 8)
  {
    serial1 ^= Common::BitCastPtr<u32>(&raw[i + 0]);
    serial2 ^= Common::BitCastPtr<u32>(&raw[i + 4]);
  }

  return std::make_pair(serial1, serial2);
}

DEntry::DEntry()
{
  memset(this, 0xFF, DENTRY_SIZE);
}

std::string DEntry::GCI_FileName() const
{
  std::string filename =
      std::string(reinterpret_cast<const char*>(m_makercode.data()), m_makercode.size()) + '-' +
      std::string(reinterpret_cast<const char*>(m_gamecode.data()), m_gamecode.size()) + '-' +
      reinterpret_cast<const char*>(m_filename.data()) + ".gci";
  return Common::EscapeFileName(filename);
}

void Header::FixChecksums()
{
  std::tie(m_checksum, m_checksum_inv) = CalculateChecksums();
}

std::pair<u16, u16> Header::CalculateChecksums() const
{
  static_assert(std::is_trivially_copyable<Header>());

  std::array<u8, sizeof(Header)> raw;
  memcpy(raw.data(), this, raw.size());

  constexpr size_t checksum_area_start = offsetof(Header, m_serial);
  constexpr size_t checksum_area_end = offsetof(Header, m_checksum);
  constexpr size_t checksum_area_size = checksum_area_end - checksum_area_start;
  return CalculateMemcardChecksums(&raw[checksum_area_start], checksum_area_size);
}

GCMemcardErrorCode Header::CheckForErrors(u16 card_size_mbits) const
{
  GCMemcardErrorCode error_code;

  // total card size should match card size in header
  if (m_size_mb != card_size_mbits)
    error_code.Set(GCMemcardValidityIssues::MISMATCHED_CARD_SIZE);

  // unused areas, should always be filled with 0xFF
  if (std::any_of(m_unused_1.begin(), m_unused_1.end(), [](u8 val) { return val != 0xFF; }) ||
      std::any_of(m_unused_2.begin(), m_unused_2.end(), [](u8 val) { return val != 0xFF; }))
  {
    error_code.Set(GCMemcardValidityIssues::DATA_IN_UNUSED_AREA);
  }

  // verify checksums
  const auto [checksum_sum, checksum_inv] = CalculateChecksums();
  if (checksum_sum != m_checksum || checksum_inv != m_checksum_inv)
    error_code.Set(GCMemcardValidityIssues::INVALID_CHECKSUM);

  return error_code;
}

Directory::Directory()
{
  memset(this, 0xFF, BLOCK_SIZE);
  m_update_counter = 0;
  m_checksum = BE16(0xF003);
  m_checksum_inv = 0;
}

bool Directory::Replace(const DEntry& entry, size_t index)
{
  if (index >= m_dir_entries.size())
    return false;

  m_dir_entries[index] = entry;
  FixChecksums();
  return true;
}

void Directory::FixChecksums()
{
  std::tie(m_checksum, m_checksum_inv) = CalculateChecksums();
}

std::pair<u16, u16> Directory::CalculateChecksums() const
{
  static_assert(std::is_trivially_copyable<Directory>());

  std::array<u8, sizeof(Directory)> raw;
  memcpy(raw.data(), this, raw.size());

  constexpr size_t checksum_area_start = offsetof(Directory, m_dir_entries);
  constexpr size_t checksum_area_end = offsetof(Directory, m_checksum);
  constexpr size_t checksum_area_size = checksum_area_end - checksum_area_start;
  return CalculateMemcardChecksums(&raw[checksum_area_start], checksum_area_size);
}

GCMemcardErrorCode Directory::CheckForErrors() const
{
  GCMemcardErrorCode error_code;

  // verify checksums
  const auto [checksum_sum, checksum_inv] = CalculateChecksums();
  if (checksum_sum != m_checksum || checksum_inv != m_checksum_inv)
    error_code.Set(GCMemcardValidityIssues::INVALID_CHECKSUM);

  // unused area, should always be filled with 0xFF
  if (std::any_of(m_padding.begin(), m_padding.end(), [](u8 val) { return val != 0xFF; }))
    error_code.Set(GCMemcardValidityIssues::DATA_IN_UNUSED_AREA);

  return error_code;
}

#include "stdafx.h"
#include "CapcomSnesScanner.h"
#include "CapcomSnesSeq.h"
#include "SNESDSP.h"

using namespace std;

// ; Super Ghouls 'N Ghosts SPC
// 03f5: 1c        asl   a
// 03f6: 5d        mov   x,a
// 03f7: f5 03 0e  mov   a,$0e03+x         ; NOTE: some games read LSB first!
// 03fa: c4 c0     mov   $c0,a
// 03fc: f5 02 0e  mov   a,$0e02+x         ; read song header address from song list
// 03ff: c4 c1     mov   $c1,a
// 0401: 04 c0     or    a,$c0
// 0403: f0 dd     beq   $03e2             ; return if song header address == $0000
BytePattern CapcomSnesScanner::ptnReadSongList(
	"\x1c\x5d\xf5\x03\x0e\xc4\xc0\xf5"
	"\x02\x0e\xc4\xc1\x04\xc0\xf0\xdd"
	,
	"xxx??x?x"
	"??x?x?x?"
	,
	16);

//; Mega Man X SPC
// 059f: 6f        ret
// 
// 05a0: 3f ef 06  call  $06ef
// 05a3: 8f 0d a1  mov   $a1,#$0d
// 05a6: 8f af a0  mov   $a0,#$af          ; song address = $0daf (+1 for actual start address)
// ; fall through...
// ; or when song header first byte == 0:
// 05a9: 3f 82 05  call  $0582
// 05ac: 8d 00     mov   y,#$00
// 05ae: dd        mov   a,y
BytePattern CapcomSnesScanner::ptnReadBGMAddress(
	"\x6f\x3f\xef\x06\x8f\x0d\xa1\x8f"
	"\xaf\xa0\x3f\x82\x05\x8d\x00\xdd"
	,
	"xx??x??x"
	"??x??xxx"
	,
	16);

void CapcomSnesScanner::Scan(RawFile* file, void* info)
{
	uint32_t nFileLength = file->size();
	if (nFileLength == 0x10000)
	{
		SearchForCapcomSnesFromARAM(file);
	}
	else
	{
		SearchForCapcomSnesFromROM(file);
	}
	return;
}

void CapcomSnesScanner::SearchForCapcomSnesFromARAM (RawFile* file)
{
	CapcomSnesVersion version = NONE;

	UINT ofsReadSongListASM;
	UINT ofsReadBGMAddressASM;
	bool hasSongList;
	bool bgmAtFixedAddress;
	UINT addrSongList;
	UINT addrBGMHeader;

	wstring basefilename = RawFile::removeExtFromPath(file->GetFileName());
	wstring name = file->tag.HasTitle() ? file->tag.title : basefilename;

	// find a song list
	hasSongList = file->SearchBytePattern(ptnReadSongList, ofsReadSongListASM);
	if (hasSongList)
	{
		addrSongList = min(file->GetShort(ofsReadSongListASM + 3), file->GetShort(ofsReadSongListASM + 8));
	}

	// find BGM address
	bgmAtFixedAddress = file->SearchBytePattern(ptnReadBGMAddress, ofsReadBGMAddressASM);
	if (bgmAtFixedAddress)
	{
		addrBGMHeader = (file->GetByte(ofsReadBGMAddressASM + 5) << 8) | file->GetByte(ofsReadBGMAddressASM + 8);
	}

	// guess engine version
	if (hasSongList)
	{
		if (bgmAtFixedAddress)
		{
			version = V2_BGM_USUALLY_AT_FIXED_LOCATION;

			// Some games still use BGM/SFX list, apparently.
			// - The Magical Quest Starring Mickey Mouse
			// - Captain Commando
			bool bgmHeaderCoversSongList = (addrBGMHeader <= addrSongList && addrBGMHeader + 17 > addrSongList);
			if (bgmHeaderCoversSongList || !IsValidBGMHeader(file, addrBGMHeader))
			{
				bgmAtFixedAddress = false;
			}
		}
		else
		{
			version = V1_BGM_IN_LIST;
		}
	}
	else
	{
		version = V3_BGM_FIXED_LOCATION;
	}

	// load a sequence from BGM region
	if (bgmAtFixedAddress)
	{
		CapcomSnesSeq* newSeq = new CapcomSnesSeq(file, version, addrBGMHeader + 1, false, name);
		if (!newSeq->LoadVGMFile())
		{
			delete newSeq;
			return;
		}
	}

	// load songs from list
	if (hasSongList)
	{
		// guess current song number
		int8_t guessedSongIndex = -1;
		if (!bgmAtFixedAddress)
		{
			guessedSongIndex = GuessCurrentSongFromARAM(file, version, addrSongList);
		}

		bool loadOnlyCurrentSong = false;
		if (loadOnlyCurrentSong)
		{
			// load current song if possible
			if (guessedSongIndex != -1)
			{
				uint16_t addrSongHeader = file->GetShortBE(addrSongList + guessedSongIndex * 2);
				CapcomSnesSeq* newSeq = new CapcomSnesSeq(file, version, addrSongHeader, true, name);
				if (!newSeq->LoadVGMFile())
				{
					delete newSeq;
					return;
				}
			}
		}
		else
		{
			// load all songs in the list
			for (int songIndex = 0; songIndex < GetLengthOfSongList(file, addrSongList); songIndex++)
			{
				uint16_t addrSongHeader = file->GetShortBE(addrSongList + songIndex * 2);
				if (addrSongHeader == 0)
				{
					continue;
				}

				CapcomSnesSeq* newSeq = new CapcomSnesSeq(file, version, addrSongHeader, true, (songIndex == guessedSongIndex) ? name : basefilename);
				if (!newSeq->LoadVGMFile())
				{
					delete newSeq;
					return;
				}
			}
		}
	}

	// TODO: scan samples
}

void CapcomSnesScanner::SearchForCapcomSnesFromROM (RawFile* file)
{
}

uint16_t CapcomSnesScanner::GetCurrentPlayAddressFromARAM (RawFile* file, CapcomSnesVersion version, uint8_t channel)
{
	uint16_t currentAddress;
	if (version == V1_BGM_IN_LIST)
	{
		currentAddress = file->GetByte(0x00 + channel * 2 + 1) | (file->GetByte(0x10 + channel * 2 + 1) << 8);
	}
	else
	{
		currentAddress = file->GetByte(0x00 + channel) | (file->GetByte(0x08 + channel) << 8);
	}
	return currentAddress;
}

int CapcomSnesScanner::GetLengthOfSongList (RawFile* file, uint16_t addrSongList)
{
	int length = 0;

	// do heuristic search for each songs
	for (int8_t songIndex = 0; songIndex <= 0x7f; songIndex++)
	{
		// check the address range of song pointer
		if (addrSongList + songIndex * 2 + 2 > 0x10000)
		{
			break;
		}

		// get header address and validate it
		uint16_t addrSongHeader = file->GetShortBE(addrSongList + songIndex * 2);
		if (addrSongHeader == 0)
		{
			length++;
			continue;
		}
		else if (!IsValidBGMHeader(file, addrSongHeader))
		{
			break;
		}

		length++;
	}

	return length;
}

int8_t CapcomSnesScanner::GuessCurrentSongFromARAM (RawFile* file, CapcomSnesVersion version, uint16_t addrSongList)
{
	int8_t guessedSongIndex = -1;
	int guessBestScore = INT_MAX;

	// do heuristic search for each songs
	for (int songIndex = 0; songIndex < GetLengthOfSongList(file, addrSongList); songIndex++)
	{
		// get header address
		uint16_t addrSongHeader = file->GetShortBE(addrSongList + songIndex * 2);
		if (addrSongHeader == 0)
		{
			continue;
		}

		// read start address for each voices
		int guessScore = 0;
		int validTrackCount = 0;
		for (int track = 0; track < 8; track++)
		{
			uint16_t addrScoreData = file->GetShortBE(addrSongHeader + 1 + track * 2);
			uint16_t currentAddress = this->GetCurrentPlayAddressFromARAM(file, version, 7 - track);

			// next if the voice is stopped, or not loaded yet
			if (currentAddress == 0)
			{
				continue;
			}

			// current address must be greater than start address
			if (addrScoreData > currentAddress)
			{
				validTrackCount = 0;
				break;
			}

			// measure the distance
			guessScore += (currentAddress - addrScoreData);
			validTrackCount++;
		}

		// update search result if necessary
		if (validTrackCount > 0)
		{
			// calculate the average score of all tracks
			// (also, make it a fixed-point number)
			guessScore = (guessScore * 16) / validTrackCount;
			if (guessBestScore > guessScore)
			{
				guessBestScore = guessScore;
				guessedSongIndex = songIndex;
			}
		}
	}

	return guessedSongIndex;
}

bool CapcomSnesScanner::IsValidBGMHeader (RawFile* file, UINT addrSongHeader)
{
	if (addrSongHeader + 17 > 0x10000)
	{
		return false;
	}

	for (int track = 0; track < 8; track++)
	{
		uint16_t addrScoreData = file->GetShortBE(addrSongHeader + 1 + track * 2);
		if ((addrScoreData & 0xff00) == 0) {
			return false;
		}
	}

	return true;
}

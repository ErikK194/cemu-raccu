#include "Cafe/HW/Latte/TexturePack/TexPackDecode.h"

namespace texpack
{
	namespace
	{
		inline uint32_t ReadLE16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
		inline uint32_t ReadLE32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

		inline void Unpack565(uint32_t c, uint32_t& r, uint32_t& g, uint32_t& b)
		{
			const uint32_t r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
			r = (r5 << 3) | (r5 >> 2);
			g = (g6 << 2) | (g6 >> 4);
			b = (b5 << 3) | (b5 >> 2);
		}

		// colour part shared by BC1 and BC2; threeColorMode only exists in BC1
		void DecodeColorBlock(const uint8_t* block, uint8_t rgbaOut[64], bool allowThreeColorMode)
		{
			const uint32_t c0 = ReadLE16(block + 0);
			const uint32_t c1 = ReadLE16(block + 2);
			const uint32_t bits = ReadLE32(block + 4);
			uint32_t pal[4][4];
			Unpack565(c0, pal[0][0], pal[0][1], pal[0][2]);
			Unpack565(c1, pal[1][0], pal[1][1], pal[1][2]);
			pal[0][3] = pal[1][3] = 255;
			if (c0 > c1 || !allowThreeColorMode)
			{
				for (int ch = 0; ch < 3; ch++)
				{
					pal[2][ch] = (2 * pal[0][ch] + pal[1][ch]) / 3;
					pal[3][ch] = (pal[0][ch] + 2 * pal[1][ch]) / 3;
				}
				pal[2][3] = pal[3][3] = 255;
			}
			else
			{
				for (int ch = 0; ch < 3; ch++)
				{
					pal[2][ch] = (pal[0][ch] + pal[1][ch]) / 2;
					pal[3][ch] = 0;
				}
				pal[2][3] = 255;
				pal[3][3] = 0;
			}
			for (int i = 0; i < 16; i++)
			{
				const uint32_t idx = (bits >> (2 * i)) & 3;
				for (int ch = 0; ch < 4; ch++)
					rgbaOut[i * 4 + ch] = (uint8_t)pal[idx][ch];
			}
		}
	}

	void DecodeBC1Block(const uint8_t* block, uint8_t rgbaOut[64])
	{
		DecodeColorBlock(block, rgbaOut, true);
	}

	void DecodeBC2Block(const uint8_t* block, uint8_t rgbaOut[64])
	{
		DecodeColorBlock(block + 8, rgbaOut, false);
		for (int i = 0; i < 16; i++)
		{
			const uint8_t raw = block[i / 2];
			const uint32_t a4 = (i % 2 == 0) ? (raw & 0x0F) : ((raw >> 4) & 0x0F);
			rgbaOut[i * 4 + 3] = (uint8_t)((a4 << 4) | a4);
		}
	}
}

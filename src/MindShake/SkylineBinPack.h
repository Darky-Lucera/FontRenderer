#pragma once

//-------------------------------------
// Original code from Jukka Jylänki
//
// Modified by Carlos Aragonés
//-------------------------------------

#include <cstdint>
#include <vector>


//-------------------------------------
namespace MindShake {

    //---------------------------------
    class SkylineBinPack {
        using uint32_t = int32_t;
        public:
            // Heuristic rules to decide how to make the rectangle placements.
            //-------------------------
            enum class ELevelChoiceHeuristic {
                LevelBottomLeft,
                LevelMinWasteFit
            };

            //-------------------------
            struct Rect {
                Rect() = default;
                Rect(int32_t x, int32_t y, int32_t width, int32_t height) : x(x), y(y), width(width), height(height) { }

                int32_t left() const    { return x;          }
                int32_t right() const   { return x + width;  }
                int32_t top() const     { return y;          }
                int32_t bottom() const  { return y + height; }

                int32_t x;
                int32_t y;
                int32_t width;
                int32_t height;
            };

        public:
                        SkylineBinPack(bool allowRotation = true)                                  { Init(    1,      1, allowRotation); }
                        SkylineBinPack(uint32_t width, uint32_t height, bool allowRotation = true) { Init(width, height, allowRotation); }

            // (Re)initializes the packer to an empty bin of width x height units.
            void	    Init(uint32_t width, uint32_t height, bool allowRotation = true);

            // Resizes the Bin (if the new size is greatest)
            bool	    ResizeBin(uint32_t width, uint32_t height);

            // Inserts a single rectangle into the bin, possibly rotated.
            Rect	    Insert(uint32_t width, uint32_t height, ELevelChoiceHeuristic method);

            uint32_t    GetWidth() const											{ return mBinWidth;        }
            uint32_t	GetHeight()	const											{ return mBinHeight;       }
            uint32_t	GetUsedSurfaceArea() const									{ return mUsedSurfaceArea; }

        protected:
            Rect	    _InsertBottomLeft(uint32_t width, uint32_t height);
            Rect	    _FindPositionForNewNodeBottomLeft(uint32_t width, uint32_t height, uint32_t *pBestHeight, uint32_t *pBestWidth, int32_t *pBestIndex) const;
            bool	    _RectangleFits(int32_t nodeIndex, uint32_t width, uint32_t height, uint32_t *pY) const;

            Rect	    _InsertMinWaste(uint32_t width, uint32_t height);
            Rect	    _FindPositionForNewNodeMinWaste(uint32_t width, uint32_t height, uint32_t *pBestHeight, uint32_t *pBestWastedArea, int32_t *pBestIndex) const;
            bool	    _RectangleFits(int32_t nodeIndex, uint32_t width, uint32_t height, uint32_t *pY, uint32_t *pWastedArea) const;
            int		    _ComputeWastedArea(int32_t nodeIndex, uint32_t width, uint32_t height, uint32_t _y) const;

            void	    _AddSkylineLevel(int32_t nodeIndex, const Rect &rRect);
            void	    _MergeSkylines();

        protected:
            // Represents a single level (a horizontal line) of the skyline/horizon/envelope.
            //-------------------------
            struct SkylineNode {
                int32_t x;
                int32_t y;
                int32_t width;
            };

            std::vector<SkylineNode>	mSkyLine;

            uint32_t		mBinWidth;
            uint32_t		mBinHeight;
            uint32_t		mUsedSurfaceArea;
            bool		    mAllowRotation;
    };

} // end of namespace

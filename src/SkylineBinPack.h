#pragma once

//-------------------------------------
// Original code from Jukka Jylänki
//
// Modified by Carlos Aragonés
//-------------------------------------

#include <cstddef>
#include <cstdint>
#include <vector>


//-------------------------------------
namespace MindShake {

    //---------------------------------
    class SkylineBinPack {
        public:
            // Heuristic rules to decide how to make the rectangle placements.
            //-------------------------
            enum class ELevelChoiceHeuristic {
                LevelBottomLeft,
                LevelMinWasteFit
            };

            //-------------------------
            struct Size {
                uint32_t width  {};
                uint32_t height {};
            };

            //-------------------------
            struct Rect {
                int32_t left() const    { return x;          }
                int32_t right() const   { return x + width;  }
                int32_t top() const     { return y;          }
                int32_t bottom() const  { return y + height; }

                int32_t x      {};
                int32_t y      {};
                int32_t width  {};
                int32_t height {};
            };

            // Rect uses signed coordinates, so every placed rectangle must fit in int32_t.
            static constexpr uint32_t kMaxBinSize = uint32_t(INT32_MAX);

        public:
            explicit    SkylineBinPack(bool allowRotation = true) : SkylineBinPack(1, 1, allowRotation) { }
                        SkylineBinPack(uint32_t width, uint32_t height, bool allowRotation = true);

            // (Re)initializes the packer to an empty bin of width x height units.
            // Returns false and preserves the current state if a dimension is zero or greater than kMaxBinSize.
            bool        Init(uint32_t width, uint32_t height, bool allowRotation = true);
            void        Reset();

            // Only grows the bin. Returns false if any dimension would shrink or is invalid.
            bool        ResizeBin(uint32_t width, uint32_t height);

            // Inserts a single rectangle into the bin, possibly rotated. Returns an empty rectangle on failure.
            Rect        Insert(uint32_t width, uint32_t height, ELevelChoiceHeuristic method);

            // Inserts as many rectangles as possible, picking at each step the best fit among the pending ones.
            // rects[i] is the placement of sizes[i] (rotated if its width differs), or an empty rectangle if it did not fit.
            // Returns the number of rectangles placed.
            size_t      Insert(const std::vector<Size> &sizes, std::vector<Rect> &rects, ELevelChoiceHeuristic method);

            uint32_t    GetWidth() const                                            { return mBinWidth;        }
            uint32_t    GetHeight() const                                           { return mBinHeight;       }
            uint32_t    GetUsedWidth() const;
            uint32_t    GetUsedHeight() const;
            uint64_t    GetUsedSurfaceArea() const                                  { return mUsedSurfaceArea; }

        protected:
            // Scores depend on the heuristic; lower is better, compared lexicographically.
            //-------------------------
            struct Candidate {
                bool IsValid() const { return nodeIndex != SIZE_MAX; }
                bool IsBetterThan(const Candidate &other) const;

                Rect        rect;
                size_t      nodeIndex      { SIZE_MAX };
                uint64_t    primaryScore   {};
                uint64_t    secondaryScore {};
            };

            // Represents a single level (a horizontal line) of the skyline/horizon/envelope.
            //-------------------------
            struct SkylineNode {
                uint32_t x;
                uint32_t y;
                uint32_t width;
            };

        protected:
            Candidate   FindPosition(uint32_t width, uint32_t height, ELevelChoiceHeuristic method) const;
            Candidate   ScorePosition(size_t nodeIndex, uint32_t width, uint32_t height, ELevelChoiceHeuristic method) const;
            bool        RectangleFits(size_t nodeIndex, uint32_t width, uint32_t height, uint32_t &y) const;
            uint64_t    ComputeWastedArea(size_t nodeIndex, uint32_t width, uint32_t y) const;

            void        AddSkylineLevel(size_t nodeIndex, const Rect &rect);
            void        MergeSkylines();

        protected:
            std::vector<SkylineNode>    mSkyLine;

            uint32_t        mBinWidth        {};
            uint32_t        mBinHeight       {};
            uint64_t        mUsedSurfaceArea {};
            bool            mAllowRotation   {};
    };

} // end of namespace

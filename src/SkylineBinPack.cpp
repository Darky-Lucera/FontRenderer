#include "SkylineBinPack.h"
//-------------------------------------
#include <algorithm>
#include <cassert>

//-------------------------------------
namespace MindShake {

    // C++14 needs this definition whenever the constant is bound to a reference.
    constexpr uint32_t SkylineBinPack::kMaxBinSize;

    //---------------------------------
    static bool
    IsValidBinSize(uint32_t width, uint32_t height) {
        return width  > 0 && width  <= SkylineBinPack::kMaxBinSize
            && height > 0 && height <= SkylineBinPack::kMaxBinSize;
    }

    //---------------------------------
    SkylineBinPack::SkylineBinPack(uint32_t width, uint32_t height, bool allowRotation) {
        const bool initialized = Init(width, height, allowRotation);
        assert(initialized);
        if(initialized == false) {
            Init(1, 1, allowRotation);
        }
    }

    //---------------------------------
    bool
    SkylineBinPack::Init(uint32_t width, uint32_t height, bool allowRotation) {
        if(IsValidBinSize(width, height) == false) {
            return false;
        }

        mBinWidth      = width;
        mBinHeight     = height;
        mAllowRotation = allowRotation;
        Reset();

        return true;
    }

    //---------------------------------
    void
    SkylineBinPack::Reset() {
        mUsedSurfaceArea = 0;

        mSkyLine.clear();
        mSkyLine.push_back({ 0, 0, mBinWidth });
    }

    //---------------------------------
    uint32_t
    SkylineBinPack::GetUsedWidth() const {
        for(auto it = mSkyLine.rbegin(); it != mSkyLine.rend(); ++it) {
            if(it->y > 0) {
                return it->x + it->width;
            }
        }

        return 0;
    }

    //---------------------------------
    uint32_t
    SkylineBinPack::GetUsedHeight() const {
        uint32_t usedHeight = 0;

        for(const SkylineNode &node : mSkyLine)
            usedHeight = std::max(usedHeight, node.y);

        return usedHeight;
    }

    //---------------------------------
    bool
    SkylineBinPack::ResizeBin(uint32_t width, uint32_t height) {
        if(IsValidBinSize(width, height) == false) {
            return false;
        }

        if(width < mBinWidth || height < mBinHeight) {
            return false;
        }

        if(width > mBinWidth) {
            mSkyLine.push_back({ mBinWidth, 0, width - mBinWidth });
            MergeSkylines();
        }

        mBinWidth  = width;
        mBinHeight = height;

        return true;
    }

    //---------------------------------
    SkylineBinPack::Rect
    SkylineBinPack::Insert(uint32_t width, uint32_t height, ELevelChoiceHeuristic method) {
        if(width == 0 || height == 0) {
            return {};
        }

        const Candidate best = FindPosition(width, height, method);
        if(best.IsValid() == false) {
            return {};
        }

        AddSkylineLevel(best.nodeIndex, best.rect);

        return best.rect;
    }

    //---------------------------------
    size_t
    SkylineBinPack::Insert(const std::vector<Size> &sizes, std::vector<Rect> &rects, ELevelChoiceHeuristic method) {
        rects.assign(sizes.size(), Rect {});

        std::vector<size_t> pending;
        pending.reserve(sizes.size());
        for(size_t i = 0; i < sizes.size(); ++i) {
            if(sizes[i].width > 0 && sizes[i].height > 0) {
                pending.push_back(i);
            }
        }

        size_t placed = 0;
        while(pending.empty() == false) {
            Candidate   best;
            size_t      bestPending = 0;

            for(size_t i = 0; i < pending.size(); ++i) {
                const Size      &size     = sizes[pending[i]];
                const Candidate candidate = FindPosition(size.width, size.height, method);
                if(candidate.IsBetterThan(best)) {
                    best        = candidate;
                    bestPending = i;
                }
            }

            if(best.IsValid() == false) {
                break;
            }

            AddSkylineLevel(best.nodeIndex, best.rect);
            rects[pending[bestPending]] = best.rect;
            pending.erase(pending.begin() + std::ptrdiff_t(bestPending));
            ++placed;
        }

        return placed;
    }

    //---------------------------------
    bool
    SkylineBinPack::Candidate::IsBetterThan(const Candidate &other) const {
        if(IsValid() == false) {
            return false;
        }

        if(other.IsValid() == false) {
            return true;
        }

        return primaryScore < other.primaryScore
            || (primaryScore == other.primaryScore && secondaryScore < other.secondaryScore);
    }

    //---------------------------------
    SkylineBinPack::Candidate
    SkylineBinPack::FindPosition(uint32_t width, uint32_t height, ELevelChoiceHeuristic method) const {
        Candidate best;

        for(size_t i = 0; i < mSkyLine.size(); ++i) {
            const Candidate candidate = ScorePosition(i, width, height, method);
            if(candidate.IsBetterThan(best)) {
                best = candidate;
            }

            if(mAllowRotation) {
                const Candidate rotated = ScorePosition(i, height, width, method);
                if(rotated.IsBetterThan(best)) {
                    best = rotated;
                }
            }
        }

        return best;
    }

    //---------------------------------
    SkylineBinPack::Candidate
    SkylineBinPack::ScorePosition(size_t nodeIndex, uint32_t width, uint32_t height, ELevelChoiceHeuristic method) const {
        Candidate   candidate;
        uint32_t    y;

        if(RectangleFits(nodeIndex, width, height, y) == false) {
            return candidate;
        }

        switch(method) {
            case ELevelChoiceHeuristic::LevelBottomLeft:
                candidate.primaryScore   = y + height;
                candidate.secondaryScore = mSkyLine[nodeIndex].width;
                break;

            case ELevelChoiceHeuristic::LevelMinWasteFit:
                candidate.primaryScore   = ComputeWastedArea(nodeIndex, width, y);
                candidate.secondaryScore = y + height;
                break;

            default:
                return candidate;
        }

        candidate.rect      = { int32_t(mSkyLine[nodeIndex].x), int32_t(y), int32_t(width), int32_t(height) };
        candidate.nodeIndex = nodeIndex;

        return candidate;
    }

    //---------------------------------
    bool
    SkylineBinPack::RectangleFits(size_t nodeIndex, uint32_t width, uint32_t height, uint32_t &y) const {
        assert(nodeIndex < mSkyLine.size());
        assert(mSkyLine[nodeIndex].x < mBinWidth);

        if(width > mBinWidth - mSkyLine[nodeIndex].x) {
            return false;
        }

        uint32_t widthLeft = width;
        y = 0;
        for(size_t i = nodeIndex; i < mSkyLine.size(); ++i) {
            y = std::max(y, mSkyLine[i].y);
            if(height > mBinHeight - y) {
                return false;
            }

            if(mSkyLine[i].width >= widthLeft) {
                return true;
            }

            widthLeft -= mSkyLine[i].width;
        }

        return false;
    }

    //---------------------------------
    uint64_t
    SkylineBinPack::ComputeWastedArea(size_t nodeIndex, uint32_t width, uint32_t y) const {
        uint64_t        wastedArea = 0;
        const uint32_t  rectRight  = mSkyLine[nodeIndex].x + width;

        for(size_t i = nodeIndex; i < mSkyLine.size() && mSkyLine[i].x < rectRight; ++i) {
            const SkylineNode &node = mSkyLine[i];
            const uint32_t    right = std::min(rectRight, node.x + node.width);

            assert(y >= node.y);
            wastedArea += uint64_t(right - node.x) * uint64_t(y - node.y);
        }

        return wastedArea;
    }

    //---------------------------------
    void
    SkylineBinPack::AddSkylineLevel(size_t nodeIndex, const Rect &rect) {
        assert(rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0);

        const SkylineNode newNode { uint32_t(rect.x), uint32_t(rect.bottom()), uint32_t(rect.width) };
        assert(newNode.width <= mBinWidth - newNode.x);
        assert(newNode.y <= mBinHeight);

        const auto     insertPosition = mSkyLine.insert(mSkyLine.begin() + std::ptrdiff_t(nodeIndex), newNode);
        const uint32_t newRight       = newNode.x + newNode.width;

        // The new level covers some of the following nodes: remove the hidden ones and trim the last partially covered one.
        const auto first = insertPosition + 1;
        auto       last  = first;
        while(last != mSkyLine.end() && last->x + last->width <= newRight)
            ++last;

        if(last != mSkyLine.end() && last->x < newRight) {
            last->width -= newRight - last->x;
            last->x      = newRight;
        }

        mSkyLine.erase(first, last);
        MergeSkylines();

        mUsedSurfaceArea += uint64_t(rect.width) * uint64_t(rect.height);
    }

    //---------------------------------
    void
    SkylineBinPack::MergeSkylines() {
        for(size_t i = 0; i + 1 < mSkyLine.size();) {
            if(mSkyLine[i].y == mSkyLine[i + 1].y) {
                mSkyLine[i].width += mSkyLine[i + 1].width;
                mSkyLine.erase(mSkyLine.begin() + std::ptrdiff_t(i + 1));
            }
            else {
                ++i;
            }
        }
    }

} // end of namespace

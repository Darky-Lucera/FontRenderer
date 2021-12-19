#include "SkylineBinPack.h"
//-------------------------------------
#include <cassert>
#include <cmath>
#include <cstring>
#include <algorithm>

//-------------------------------------
namespace MindShake {

    //---------------------------------
    void
    SkylineBinPack::Init(uint32_t width, uint32_t height, bool allowRotation) {
        SkylineNode	node;

        mBinWidth        = width;
        mBinHeight       = height;
        mAllowRotation   = allowRotation;
        mUsedSurfaceArea = 0;

        mSkyLine.clear();

        node.x     = 0;
        node.y     = 0;
        node.width = width;
        mSkyLine.push_back(node);
    }

    //---------------------------------
    bool
    SkylineBinPack::ResizeBin(uint32_t width, uint32_t height) {
        SkylineNode node;

        if(width < mBinWidth || height < mBinHeight)
            return false;

        if(width > mBinWidth) {
            node.x     = mBinWidth;
            node.y     = 0;
            node.width = width - mBinWidth;

            mSkyLine.push_back(node);
        }

        mBinWidth  = width;
        mBinHeight = height;

        return true;
    }

    //---------------------------------
    SkylineBinPack::Rect
    SkylineBinPack::Insert(uint32_t width, uint32_t height, ELevelChoiceHeuristic method) {

        switch(method) {
            case ELevelChoiceHeuristic::LevelBottomLeft:
                return _InsertBottomLeft(width, height);

            case ELevelChoiceHeuristic::LevelMinWasteFit:
                return _InsertMinWaste(width, height);
        }

        return { 0, 0, 0, 0 };
    }


    //---------------------------------
    // Bottom Left Heuristic
    //---------------------------------

    //---------------------------------
    SkylineBinPack::Rect
    SkylineBinPack::_InsertBottomLeft(uint32_t width, uint32_t height) {
        uint32_t	bestHeight;
        uint32_t	bestWidth;
        int32_t		bestIndex;
        Rect		newNode;

        newNode = _FindPositionForNewNodeBottomLeft(width, height, &bestHeight, &bestWidth, &bestIndex);
        if(bestIndex != -1) {
            _AddSkylineLevel(bestIndex, newNode);

            mUsedSurfaceArea += width * height;
        }
        else {
            memset(&newNode, 0, sizeof(Rect));
        }

        return newNode;
    }

    //---------------------------------
    SkylineBinPack::Rect
    SkylineBinPack::_FindPositionForNewNodeBottomLeft(uint32_t width, uint32_t height, uint32_t *pBestHeight, uint32_t *pBestWidth, int32_t *pBestIndex) const {
        Rect		newNode;
        uint32_t	y;

        *pBestIndex  = -1;
        *pBestWidth  = 0x7fffffff;
        *pBestHeight = 0x7fffffff;

        memset(&newNode, 0, sizeof(newNode));

        for(size_t i = 0; i < mSkyLine.size(); ++i) {
            if(_RectangleFits(int(i), width, height, &y)) {
                if((y + height < (*pBestHeight)) || ((y + height == (*pBestHeight)) && (mSkyLine[i].width < (*pBestWidth)))) {
                    *pBestIndex  = int(i);
                    *pBestWidth  = mSkyLine[i].width;
                    *pBestHeight = y + height;

                    newNode.x      = mSkyLine[i].x;
                    newNode.y      = y;
                    newNode.width  = width;
                    newNode.height = height;
                }
            }

            // Rotated
            if(mAllowRotation) {
                if(_RectangleFits(int(i), height, width, &y)) {
                    if((y + width < (*pBestHeight)) || ((y + width == *(pBestHeight)) && (mSkyLine[i].width < (*pBestWidth)))) {
                        *pBestIndex  = int(i);
                        *pBestWidth  = mSkyLine[i].width;
                        *pBestHeight = y + width;

                        newNode.x      = mSkyLine[i].x;
                        newNode.y      = y;
                        newNode.width  = height;
                        newNode.height = width;
                    }
                }
            }
        }

        return newNode;
    }

    //---------------------------------
    bool
    SkylineBinPack::_RectangleFits(int32_t nodeIndex, uint32_t width, uint32_t height, uint32_t *pY) const {
        uint32_t	x;
        int32_t		widthLeft;
        int32_t		i;

        x = mSkyLine[nodeIndex].x;
        if(x + width > mBinWidth)
            return false;

        widthLeft = width;
        i         = nodeIndex;
        (*pY)    = mSkyLine[nodeIndex].y;

        while(widthLeft > 0) {
            if((*pY) < mSkyLine[i].y)
                (*pY) = mSkyLine[i].y;

            if((*pY) + height > mBinHeight)
                return false;

            widthLeft -= mSkyLine[i].width;
            ++i;

            assert(i < int(mSkyLine.size()) || (widthLeft <= 0));
        }

        return true;
    }

    //---------------------------------
    // Min Waste Heuristic
    //---------------------------------

    //---------------------------------
    SkylineBinPack::Rect
    SkylineBinPack::_InsertMinWaste(uint32_t width, uint32_t height) {
        uint32_t	bestHeight;
        uint32_t	bestWastedArea;
        int32_t		bestIndex;
        Rect		newNode;

        newNode = _FindPositionForNewNodeMinWaste(width, height, &bestHeight, &bestWastedArea, &bestIndex);

        if(bestIndex != -1) {
            _AddSkylineLevel(bestIndex, newNode);

            mUsedSurfaceArea += width * height;
        }
        else {
            memset(&newNode, 0, sizeof(newNode));
        }

        return newNode;
    }

    //---------------------------------
    SkylineBinPack::Rect
    SkylineBinPack::_FindPositionForNewNodeMinWaste(uint32_t width, uint32_t height, uint32_t *pBestHeight, uint32_t *pBestWastedArea, int32_t *pBestIndex) const {
        Rect		newNode;
        uint32_t	y;
        uint32_t	wastedArea;

        *pBestHeight     = 0x7fffffff;
        *pBestWastedArea = 0x7fffffff;
        *pBestIndex      = -1;

        memset(&newNode, 0, sizeof(newNode));
        for(size_t i = 0; i < mSkyLine.size(); ++i) {
            if(_RectangleFits(int(i), width, height, &y, &wastedArea)) {
                if((wastedArea < (*pBestWastedArea)) || ((wastedArea == (*pBestWastedArea)) && (y + height < (*pBestHeight)))) {
                    *pBestHeight     = y + height;
                    *pBestWastedArea = wastedArea;
                    *pBestIndex      = int(i);

                    newNode.x      = mSkyLine[i].x;
                    newNode.y      = y;
                    newNode.width  = width;
                    newNode.height = height;
                }
            }

            // Rotated
            if(mAllowRotation) {
                if(_RectangleFits(int(i), height, width, &y, &wastedArea)) {
                    if((wastedArea < (*pBestWastedArea)) || ((wastedArea == (*pBestWastedArea)) && (y + width < (*pBestHeight)))) {
                        *pBestHeight     = y + width;
                        *pBestWastedArea = wastedArea;
                        *pBestIndex      = int(i);

                        newNode.x      = mSkyLine[i].x;
                        newNode.y      = y;
                        newNode.width  = height;
                        newNode.height = width;
                    }
                }
            }
        }

        return newNode;
    }

    //---------------------------------
    bool
    SkylineBinPack::_RectangleFits(int32_t nodeIndex, uint32_t width, uint32_t height, uint32_t *pY, uint32_t *pWastedArea) const {
        bool fits;

        fits = _RectangleFits(nodeIndex, width, height, pY);
        if(fits) {
            *pWastedArea = _ComputeWastedArea(nodeIndex, width, height, *pY);
        }

        return fits;
    }

    //---------------------------------
    int
    SkylineBinPack::_ComputeWastedArea(int32_t nodeIndex, uint32_t width, uint32_t height, uint32_t _y) const {
        uint32_t		wastedArea = 0;
        const uint32_t	rectLeft   = mSkyLine[nodeIndex].x;
        const uint32_t	rectRight  = rectLeft + width;
        uint32_t		leftSide, rightSide;

        for(; (nodeIndex < int(mSkyLine.size())) && (mSkyLine[nodeIndex].x < rectRight); ++nodeIndex) {
            if((mSkyLine[nodeIndex].x >= rectRight) || (mSkyLine[nodeIndex].x + mSkyLine[nodeIndex].width <= rectLeft))
                break;

            leftSide  = mSkyLine[nodeIndex].x;
            rightSide = std::min(rectRight, leftSide + mSkyLine[nodeIndex].width);
            assert(_y >= mSkyLine[nodeIndex].y);
            wastedArea += (rightSide - leftSide) * (_y - mSkyLine[nodeIndex].y);
        }

        return wastedArea;
    }

    //---------------------------------

    //---------------------------------
    void
    SkylineBinPack::_AddSkylineLevel(int32_t nodeIndex, const Rect &rRect) {
        SkylineNode	newNode;
        uint32_t	shrink;

        newNode.x     = rRect.x;
        newNode.y     = rRect.y + rRect.height;
        newNode.width = rRect.width;

        mSkyLine.insert(mSkyLine.begin() + nodeIndex, newNode);

        assert(newNode.x + newNode.width <= mBinWidth);
        assert(newNode.y <= mBinHeight);

        for(size_t i = nodeIndex+1; i < mSkyLine.size(); ++i) {
            assert(mSkyLine[i-1].x <= mSkyLine[i].x);

            if(mSkyLine[i].x < (mSkyLine[i-1].x + mSkyLine[i-1].width)) {
                shrink = mSkyLine[i-1].x + mSkyLine[i-1].width - mSkyLine[i].x;

                mSkyLine[i].x     += shrink;
                mSkyLine[i].width -= shrink;

                if(mSkyLine[i].width <= 0) {
                    mSkyLine.erase(mSkyLine.begin() + i);
                    --i;
                }
                else {
                    break;
                }
            }
            else {
                break;
            }
        }

        _MergeSkylines();
    }

    //---------------------------------
    void
    SkylineBinPack::_MergeSkylines() {
        for(size_t i = 0; i < mSkyLine.size()-1; ++i) {
            if(mSkyLine[i].y == mSkyLine[i+1].y) {
                mSkyLine[i].width += mSkyLine[i+1].width;
                mSkyLine.erase(mSkyLine.begin() + (i+1));
                --i;
            }
        }
    }

} // end of namespace

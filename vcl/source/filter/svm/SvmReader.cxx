/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include <vcl/filter/SvmReader.hxx>
#include <sal/log.hxx>
#include <osl/thread.h>
#include <tools/stream.hxx>
#include <tools/vcompat.hxx>
#include <vcl/dibtools.hxx>
#include <vcl/TypeSerializer.hxx>
#include <vcl/gdimtf.hxx>
#include <vcl/metaact.hxx>

#include <svmconverter.hxx>

namespace
{
class DepthGuard
{
private:
    ImplMetaReadData& m_rData;
    rtl_TextEncoding m_eOrigCharSet;

public:
    DepthGuard(ImplMetaReadData& rData, SvStream const& rIStm)
        : m_rData(rData)
        , m_eOrigCharSet(m_rData.meActualCharSet)
    {
        ++m_rData.mnParseDepth;
        m_rData.meActualCharSet = rIStm.GetStreamCharSet();
    }
    bool TooDeep() const { return m_rData.mnParseDepth > 1024; }
    ~DepthGuard()
    {
        --m_rData.mnParseDepth;
        m_rData.meActualCharSet = m_eOrigCharSet;
    }
};
}

SvmReader::SvmReader(SvStream& rIStm)
    : mrStream(rIStm)
{
}

SvStream& SvmReader::Read(GDIMetaFile& rMetaFile, ImplMetaReadData* pData)
{
    if (mrStream.GetError())
    {
        SAL_WARN("vcl.gdi", "Stream error: " << mrStream.GetError());
        return mrStream;
    }

    sal_uLong nStmPos = mrStream.Tell();
    SvStreamEndian nOldFormat = mrStream.GetEndian();

    mrStream.SetEndian(SvStreamEndian::LITTLE);

    try
    {
        char aId[7];
        aId[0] = 0;
        aId[6] = 0;
        mrStream.ReadBytes(aId, 6);
        if (!strcmp(aId, "VCLMTF"))
        {
            // new format
            sal_uInt32 nStmCompressMode = 0;
            sal_uInt32 nCount = 0;
            std::unique_ptr<VersionCompatRead> pCompat(new VersionCompatRead(mrStream));

            mrStream.ReadUInt32(nStmCompressMode);
            TypeSerializer aSerializer(mrStream);
            MapMode aMapMode;
            aSerializer.readMapMode(aMapMode);
            rMetaFile.SetPrefMapMode(aMapMode);
            Size aSize;
            aSerializer.readSize(aSize);
            rMetaFile.SetPrefSize(aSize);
            mrStream.ReadUInt32(nCount);

            pCompat.reset(); // destructor writes stuff into the header

            std::unique_ptr<ImplMetaReadData> xReadData;
            if (!pData)
            {
                xReadData.reset(new ImplMetaReadData);
                pData = xReadData.get();
            }
            DepthGuard aDepthGuard(*pData, mrStream);

            if (aDepthGuard.TooDeep())
                throw std::runtime_error("too much recursion");

            for (sal_uInt32 nAction = 0; (nAction < nCount) && !mrStream.eof(); nAction++)
            {
                rtl::Reference<MetaAction> pAction = MetaActionHandler(pData);
                if (pAction)
                {
                    if (pAction->GetType() == MetaActionType::COMMENT)
                    {
                        MetaCommentAction* pCommentAct
                            = static_cast<MetaCommentAction*>(pAction.get());

                        if (pCommentAct->GetComment() == "EMF_PLUS")
                            rMetaFile.UseCanvas(true);
                    }
                    rMetaFile.AddAction(pAction);
                }
            }
        }
        else
        {
            mrStream.Seek(nStmPos);
            SVMConverter(mrStream, rMetaFile);
        }
    }
    catch (...)
    {
        SAL_WARN("vcl", "GDIMetaFile exception during load");
        mrStream.SetError(SVSTREAM_FILEFORMAT_ERROR);
    };

    // check for errors
    if (mrStream.GetError())
    {
        rMetaFile.Clear();
        mrStream.Seek(nStmPos);
    }

    mrStream.SetEndian(nOldFormat);
    return mrStream;
}

rtl::Reference<MetaAction> SvmReader::MetaActionHandler(ImplMetaReadData* pData)
{
    rtl::Reference<MetaAction> pAction;
    sal_uInt16 nTmp = 0;
    mrStream.ReadUInt16(nTmp);
    MetaActionType nType = static_cast<MetaActionType>(nTmp);

    switch (nType)
    {
        case MetaActionType::NONE:
            pAction = new MetaAction;
            break;
        case MetaActionType::PIXEL:
            return PixelHandler();
            break;
        case MetaActionType::POINT:
            return PointHandler();
            break;
        case MetaActionType::LINE:
            return LineHandler();
            break;
        case MetaActionType::RECT:
            return RectHandler();
            break;
        case MetaActionType::ROUNDRECT:
            return RoundRectHandler();
            break;
        case MetaActionType::ELLIPSE:
            return EllipseHandler();
            break;
        case MetaActionType::ARC:
            return ArcHandler();
            break;
        case MetaActionType::PIE:
            return PieHandler();
            break;
        case MetaActionType::CHORD:
            return ChordHandler();
            break;
        case MetaActionType::POLYLINE:
            return PolyLineHandler();
            break;
        case MetaActionType::POLYGON:
            return PolygonHandler();
            break;
        case MetaActionType::POLYPOLYGON:
            return PolyPolygonHandler();
            break;
        case MetaActionType::TEXT:
            return TextHandler(pData);
            break;
        case MetaActionType::TEXTARRAY:
            return TextArrayHandler(pData);
            break;
        case MetaActionType::STRETCHTEXT:
            return StretchTextHandler(pData);
            break;
        case MetaActionType::TEXTRECT:
            return TextRectHandler(pData);
            break;
        case MetaActionType::TEXTLINE:
            return TextLineHandler();
            break;
        case MetaActionType::BMP:
            return BmpHandler();
            break;
        case MetaActionType::BMPSCALE:
            return BmpScaleHandler();
            break;
        case MetaActionType::BMPSCALEPART:
            return BmpScalePartHandler();
            break;
        case MetaActionType::BMPEX:
            return BmpExHandler();
            break;
        case MetaActionType::BMPEXSCALE:
            return BmpExScaleHandler();
            break;
        case MetaActionType::BMPEXSCALEPART:
            return BmpExScalePartHandler();
            break;
        case MetaActionType::MASK:
            return MaskHandler();
            break;
        case MetaActionType::MASKSCALE:
            return MaskScaleHandler();
            break;
        case MetaActionType::MASKSCALEPART:
            return MaskScalePartHandler();
            break;
        case MetaActionType::GRADIENT:
            return GradientHandler();
            break;
        case MetaActionType::GRADIENTEX:
            return GradientExHandler();
            break;
        case MetaActionType::HATCH:
            return HatchHandler();
            break;
        case MetaActionType::WALLPAPER:
            return WallpaperHandler();
            break;
        case MetaActionType::CLIPREGION:
            return ClipRegionHandler();
            break;
        case MetaActionType::ISECTRECTCLIPREGION:
            return ISectRectClipRegionHandler();
            break;
        case MetaActionType::ISECTREGIONCLIPREGION:
            return ISectRegionClipRegionHandler();
            break;
        case MetaActionType::MOVECLIPREGION:
            return MoveClipRegionHandler();
            break;
        case MetaActionType::LINECOLOR:
            return LineColorHandler();
            break;
        case MetaActionType::FILLCOLOR:
            return FillColorHandler();
            break;
        case MetaActionType::TEXTCOLOR:
            return TextColorHandler();
            break;
        case MetaActionType::TEXTFILLCOLOR:
            return TextFillColorHandler();
            break;
        case MetaActionType::TEXTLINECOLOR:
            return TextLineColorHandler();
            break;
        case MetaActionType::OVERLINECOLOR:
            return OverlineColorHandler();
            break;
        case MetaActionType::TEXTALIGN:
            return TextAlignHandler();
            break;
        case MetaActionType::MAPMODE:
            return MapModeHandler();
            break;
        case MetaActionType::FONT:
            return FontHandler(pData);
            break;
        case MetaActionType::PUSH:
            return PushHandler();
            break;
        case MetaActionType::POP:
            return PopHandler();
            break;
        case MetaActionType::RASTEROP:
            return RasterOpHandler();
            break;
        case MetaActionType::Transparent:
            return TransparentHandler();
            break;
        case MetaActionType::FLOATTRANSPARENT:
            pAction = new MetaFloatTransparentAction;
            break;
        case MetaActionType::EPS:
            pAction = new MetaEPSAction;
            break;
        case MetaActionType::REFPOINT:
            pAction = new MetaRefPointAction;
            break;
        case MetaActionType::COMMENT:
            pAction = new MetaCommentAction;
            break;
        case MetaActionType::LAYOUTMODE:
            pAction = new MetaLayoutModeAction;
            break;
        case MetaActionType::TEXTLANGUAGE:
            pAction = new MetaTextLanguageAction;
            break;

        default:
        {
            VersionCompatRead aCompat(mrStream);
        }
        break;
    }

    if (pAction)
        pAction->Read(mrStream, pData);

    return pAction;
}

void SvmReader::ReadColor(Color& rColor)
{
    sal_uInt32 nTmp;
    mrStream.ReadUInt32(nTmp);
    rColor = ::Color(ColorTransparency, nTmp);
}

rtl::Reference<MetaAction> SvmReader::LineColorHandler()
{
    auto pAction = new MetaLineColorAction();

    VersionCompatRead aCompat(mrStream);
    Color aColor;
    ReadColor(aColor);
    bool aBool;
    mrStream.ReadCharAsBool(aBool);

    pAction->SetSetting(aBool);
    pAction->SetColor(aColor);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::FillColorHandler()
{
    auto pAction = new MetaFillColorAction();

    VersionCompatRead aCompat(mrStream);

    Color aColor;
    ReadColor(aColor);
    bool aBool;
    mrStream.ReadCharAsBool(aBool);

    pAction->SetColor(aColor);
    pAction->SetSetting(aBool);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::RectHandler()
{
    auto pAction = new MetaRectAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);

    tools::Rectangle aRectangle;
    aSerializer.readRectangle(aRectangle);
    pAction->SetRect(aRectangle);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::PointHandler()
{
    auto pAction = new MetaPointAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);

    Point aPoint;
    aSerializer.readPoint(aPoint);
    pAction->SetPoint(aPoint);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::PixelHandler()
{
    auto pAction = new MetaPixelAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);

    Point aPoint;
    aSerializer.readPoint(aPoint);
    Color aColor;
    ReadColor(aColor);

    pAction->SetPoint(aPoint);
    pAction->SetColor(aColor);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::LineHandler()
{
    auto pAction = new MetaLineAction();

    VersionCompatRead aCompat(mrStream);

    // Version 1
    TypeSerializer aSerializer(mrStream);
    Point aPoint;
    Point aEndPoint;
    aSerializer.readPoint(aPoint);
    aSerializer.readPoint(aEndPoint);

    pAction->SetStartPoint(aPoint);
    pAction->SetEndPoint(aEndPoint);

    // Version 2
    if (aCompat.GetVersion() >= 2)
    {
        LineInfo aLineInfo;
        ReadLineInfo(mrStream, aLineInfo);
        pAction->SetLineInfo(aLineInfo);
    }

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::RoundRectHandler()
{
    auto pAction = new MetaRoundRectAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);

    tools::Rectangle aRectangle;
    aSerializer.readRectangle(aRectangle);
    sal_uInt32 HorzRound;
    sal_uInt32 VertRound;
    mrStream.ReadUInt32(HorzRound).ReadUInt32(VertRound);

    pAction->SetRect(aRectangle);
    pAction->SetHorzRound(HorzRound);
    pAction->SetVertRound(VertRound);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::EllipseHandler()
{
    auto pAction = new MetaEllipseAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);

    tools::Rectangle aRectangle;
    aSerializer.readRectangle(aRectangle);

    pAction->SetRect(aRectangle);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::ArcHandler()
{
    auto pAction = new MetaArcAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);

    tools::Rectangle aRectangle;
    aSerializer.readRectangle(aRectangle);
    Point aPoint;
    aSerializer.readPoint(aPoint);
    Point aEndPoint;
    aSerializer.readPoint(aEndPoint);

    pAction->SetRect(aRectangle);
    pAction->SetStartPoint(aPoint);
    pAction->SetEndPoint(aEndPoint);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::PieHandler()
{
    auto pAction = new MetaPieAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);

    tools::Rectangle aRectangle;
    aSerializer.readRectangle(aRectangle);
    Point aPoint;
    aSerializer.readPoint(aPoint);
    Point aEndPoint;
    aSerializer.readPoint(aEndPoint);

    pAction->SetRect(aRectangle);
    pAction->SetStartPoint(aPoint);
    pAction->SetEndPoint(aEndPoint);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::ChordHandler()
{
    auto pAction = new MetaChordAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);

    tools::Rectangle aRectangle;
    aSerializer.readRectangle(aRectangle);
    Point aPoint;
    aSerializer.readPoint(aPoint);
    Point aEndPoint;
    aSerializer.readPoint(aEndPoint);

    pAction->SetRect(aRectangle);
    pAction->SetStartPoint(aPoint);
    pAction->SetEndPoint(aEndPoint);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::PolyLineHandler()
{
    auto pAction = new MetaPolyLineAction();

    VersionCompatRead aCompat(mrStream);

    // Version 1
    tools::Polygon aPolygon;
    ReadPolygon(mrStream, aPolygon);

    // Version 2
    if (aCompat.GetVersion() >= 2)
    {
        LineInfo aLineInfo;
        ReadLineInfo(mrStream, aLineInfo);
        pAction->SetLineInfo(aLineInfo);
    }
    if (aCompat.GetVersion() >= 3)
    {
        sal_uInt8 bHasPolyFlags(0);
        mrStream.ReadUChar(bHasPolyFlags);
        if (bHasPolyFlags)
            aPolygon.Read(mrStream);
    }
    pAction->SetPolygon(aPolygon);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::PolygonHandler()
{
    auto pAction = new MetaPolygonAction();

    VersionCompatRead aCompat(mrStream);

    tools::Polygon aPolygon;
    ReadPolygon(mrStream, aPolygon); // Version 1

    if (aCompat.GetVersion() >= 2) // Version 2
    {
        sal_uInt8 bHasPolyFlags(0);
        mrStream.ReadUChar(bHasPolyFlags);
        if (bHasPolyFlags)
            aPolygon.Read(mrStream);
    }

    pAction->SetPolygon(aPolygon);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::PolyPolygonHandler()
{
    auto pAction = new MetaPolyPolygonAction();

    VersionCompatRead aCompat(mrStream);
    tools::PolyPolygon aPolyPolygon;
    ReadPolyPolygon(mrStream, aPolyPolygon); // Version 1

    if (aCompat.GetVersion() < 2) // Version 2
    {
        pAction->SetPolyPolygon(aPolyPolygon);
        return pAction;
    }

    sal_uInt16 nNumberOfComplexPolygons(0);
    mrStream.ReadUInt16(nNumberOfComplexPolygons);
    const size_t nMinRecordSize = sizeof(sal_uInt16);
    const size_t nMaxRecords = mrStream.remainingSize() / nMinRecordSize;
    if (nNumberOfComplexPolygons > nMaxRecords)
    {
        SAL_WARN("vcl.gdi", "Parsing error: " << nMaxRecords << " max possible entries, but "
                                              << nNumberOfComplexPolygons
                                              << " claimed, truncating");
        nNumberOfComplexPolygons = nMaxRecords;
    }
    for (sal_uInt16 i = 0; i < nNumberOfComplexPolygons; ++i)
    {
        sal_uInt16 nIndex(0);
        mrStream.ReadUInt16(nIndex);
        tools::Polygon aPoly;
        aPoly.Read(mrStream);
        if (nIndex >= aPolyPolygon.Count())
        {
            SAL_WARN("vcl.gdi", "svm contains polygon index " << nIndex
                                                              << " outside possible range "
                                                              << aPolyPolygon.Count());
            continue;
        }
        aPolyPolygon.Replace(aPoly, nIndex);
    }

    pAction->SetPolyPolygon(aPolyPolygon);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::TextHandler(ImplMetaReadData* pData)
{
    auto pAction = new MetaTextAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);

    Point aPoint;
    aSerializer.readPoint(aPoint);
    OUString aStr;
    aStr = mrStream.ReadUniOrByteString(pData->meActualCharSet);
    sal_uInt16 nTmpIndex(0);
    mrStream.ReadUInt16(nTmpIndex);
    sal_uInt16 nTmpLen(0);
    mrStream.ReadUInt16(nTmpLen);

    pAction->SetPoint(aPoint);
    pAction->SetIndex(nTmpIndex);
    pAction->SetLen(nTmpLen);

    if (aCompat.GetVersion() >= 2) // Version 2
        aStr = read_uInt16_lenPrefixed_uInt16s_ToOUString(mrStream);

    if (nTmpIndex + nTmpLen > aStr.getLength())
    {
        SAL_WARN("vcl.gdi", "inconsistent offset and len");
        pAction->SetIndex(0);
        pAction->SetLen(aStr.getLength());
    }

    pAction->SetText(aStr);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::TextArrayHandler(ImplMetaReadData* pData)
{
    auto pAction = new MetaTextArrayAction();

    std::unique_ptr<tools::Long[]> aArray;

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);

    Point aPoint;
    aSerializer.readPoint(aPoint);
    pAction->SetPoint(aPoint);

    OUString aStr;
    aStr = mrStream.ReadUniOrByteString(pData->meActualCharSet);
    pAction->SetText(aStr);

    sal_uInt16 nTmpIndex(0);
    mrStream.ReadUInt16(nTmpIndex);
    pAction->SetIndex(nTmpIndex);

    sal_uInt16 nTmpLen(0);
    mrStream.ReadUInt16(nTmpLen);
    pAction->SetLen(nTmpLen);

    sal_Int32 nAryLen(0);
    mrStream.ReadInt32(nAryLen);

    if (nTmpIndex + nTmpLen > aStr.getLength())
    {
        SAL_WARN("vcl.gdi", "inconsistent offset and len");
        pAction->SetIndex(0);
        pAction->SetLen(aStr.getLength());
        return pAction;
    }

    if (nAryLen)
    {
        // #i9762#, #106172# Ensure that DX array is at least mnLen entries long
        if (nTmpLen >= nAryLen)
        {
            aArray.reset(new (std::nothrow) tools::Long[nTmpLen]);
            if (aArray)
            {
                sal_Int32 i;
                sal_Int32 val;
                for (i = 0; i < nAryLen; i++)
                {
                    mrStream.ReadInt32(val);
                    aArray[i] = val;
                }
                // #106172# setup remainder
                for (; i < nTmpLen; i++)
                    aArray[i] = 0;
            }
        }
        else
        {
            return pAction;
        }
    }

    if (aCompat.GetVersion() >= 2) // Version 2
    {
        aStr = read_uInt16_lenPrefixed_uInt16s_ToOUString(mrStream);

        if (nTmpIndex + nTmpLen > aStr.getLength())
        {
            SAL_WARN("vcl.gdi", "inconsistent offset and len");
            pAction->SetIndex(0);
            pAction->SetLen(aStr.getLength());
            aArray.reset();
        }
    }

    pAction->SetDXArray(std::move(aArray));
    return pAction;
}

rtl::Reference<MetaAction> SvmReader::StretchTextHandler(ImplMetaReadData* pData)
{
    auto pAction = new MetaStretchTextAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);

    Point aPoint;
    aSerializer.readPoint(aPoint);
    OUString aStr;
    aStr = mrStream.ReadUniOrByteString(pData->meActualCharSet);
    sal_uInt32 nTmpWidth;
    mrStream.ReadUInt32(nTmpWidth);
    sal_uInt16 nTmpIndex(0);
    mrStream.ReadUInt16(nTmpIndex);
    sal_uInt16 nTmpLen(0);
    mrStream.ReadUInt16(nTmpLen);

    pAction->SetPoint(aPoint);
    pAction->SetWidth(nTmpWidth);
    pAction->SetIndex(nTmpIndex);
    pAction->SetLen(nTmpLen);

    if (aCompat.GetVersion() >= 2) // Version 2
        aStr = read_uInt16_lenPrefixed_uInt16s_ToOUString(mrStream);

    pAction->SetText(aStr);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::TextRectHandler(ImplMetaReadData* pData)
{
    auto pAction = new MetaTextRectAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);

    tools::Rectangle aRect;
    aSerializer.readRectangle(aRect);
    OUString aStr;
    aStr = mrStream.ReadUniOrByteString(pData->meActualCharSet);
    sal_uInt16 nTmp;
    mrStream.ReadUInt16(nTmp);

    pAction->SetRect(aRect);
    pAction->SetStyle(static_cast<DrawTextFlags>(nTmp));

    if (aCompat.GetVersion() >= 2) // Version 2
        aStr = read_uInt16_lenPrefixed_uInt16s_ToOUString(mrStream);

    pAction->SetText(aStr);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::TextLineHandler()
{
    auto pAction = new MetaTextLineAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);

    Point aPos;
    aSerializer.readPoint(aPos);
    sal_Int32 nTempWidth(0);
    mrStream.ReadInt32(nTempWidth);

    pAction->SetStartPoint(aPos);
    pAction->SetWidth(nTempWidth);

    sal_uInt32 nTempStrikeout(0);
    mrStream.ReadUInt32(nTempStrikeout);
    sal_uInt32 nTempUnderline(0);
    mrStream.ReadUInt32(nTempUnderline);

    pAction->SetStrikeout(static_cast<FontStrikeout>(nTempStrikeout));
    pAction->SetUnderline(static_cast<FontLineStyle>(nTempUnderline));

    if (aCompat.GetVersion() >= 2)
    {
        sal_uInt32 nTempOverline(0);
        mrStream.ReadUInt32(nTempOverline);
        pAction->SetOverline(static_cast<FontLineStyle>(nTempOverline));
    }

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::BmpHandler()
{
    auto pAction = new MetaBmpAction();

    VersionCompatRead aCompat(mrStream);
    Bitmap aBmp;
    ReadDIB(aBmp, mrStream, true);
    TypeSerializer aSerializer(mrStream);
    Point aPoint;
    aSerializer.readPoint(aPoint);

    pAction->SetBitmap(aBmp);
    pAction->SetPoint(aPoint);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::BmpScaleHandler()
{
    auto pAction = new MetaBmpScaleAction();

    VersionCompatRead aCompat(mrStream);
    Bitmap aBmp;
    ReadDIB(aBmp, mrStream, true);
    TypeSerializer aSerializer(mrStream);
    Point aPoint;
    aSerializer.readPoint(aPoint);
    Size aSz;
    aSerializer.readSize(aSz);

    pAction->SetBitmap(aBmp);
    pAction->SetPoint(aPoint);
    pAction->SetSize(aSz);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::BmpScalePartHandler()
{
    auto pAction = new MetaBmpScalePartAction();

    VersionCompatRead aCompat(mrStream);
    Bitmap aBmp;
    ReadDIB(aBmp, mrStream, true);
    TypeSerializer aSerializer(mrStream);
    Point aDestPoint;
    aSerializer.readPoint(aDestPoint);
    Size aDestSize;
    aSerializer.readSize(aDestSize);
    Point aSrcPoint;
    aSerializer.readPoint(aSrcPoint);
    Size aSrcSize;
    aSerializer.readSize(aSrcSize);

    pAction->SetBitmap(aBmp);
    pAction->SetDestPoint(aDestPoint);
    pAction->SetDestSize(aDestSize);
    pAction->SetSrcPoint(aSrcPoint);
    pAction->SetSrcSize(aSrcSize);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::BmpExHandler()
{
    auto pAction = new MetaBmpExAction();

    VersionCompatRead aCompat(mrStream);
    BitmapEx aBmpEx;
    ReadDIBBitmapEx(aBmpEx, mrStream);
    TypeSerializer aSerializer(mrStream);
    Point aPoint;
    aSerializer.readPoint(aPoint);

    pAction->SetPoint(aPoint);
    pAction->SetBitmapEx(aBmpEx);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::BmpExScaleHandler()
{
    auto pAction = new MetaBmpExScaleAction();

    VersionCompatRead aCompat(mrStream);
    BitmapEx aBmpEx;
    ReadDIBBitmapEx(aBmpEx, mrStream);
    TypeSerializer aSerializer(mrStream);
    Point aPoint;
    aSerializer.readPoint(aPoint);
    Size aSize;
    aSerializer.readSize(aSize);

    pAction->SetBitmapEx(aBmpEx);
    pAction->SetPoint(aPoint);
    pAction->SetSize(aSize);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::BmpExScalePartHandler()
{
    auto pAction = new MetaBmpExScalePartAction();

    VersionCompatRead aCompat(mrStream);
    BitmapEx aBmpEx;
    ReadDIBBitmapEx(aBmpEx, mrStream);
    TypeSerializer aSerializer(mrStream);
    Point aDstPoint;
    aSerializer.readPoint(aDstPoint);
    Size aDstSize;
    aSerializer.readSize(aDstSize);
    Point aSrcPoint;
    aSerializer.readPoint(aSrcPoint);
    Size aSrcSize;
    aSerializer.readSize(aSrcSize);

    pAction->SetBitmapEx(aBmpEx);
    pAction->SetDestPoint(aDstPoint);
    pAction->SetDestSize(aDstSize);
    pAction->SetSrcPoint(aSrcPoint);
    pAction->SetSrcSize(aSrcSize);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::MaskHandler()
{
    auto pAction = new MetaMaskAction();

    VersionCompatRead aCompat(mrStream);
    Bitmap aBmp;
    ReadDIB(aBmp, mrStream, true);
    TypeSerializer aSerializer(mrStream);
    Point aPoint;
    aSerializer.readPoint(aPoint);

    pAction->SetBitmap(aBmp);
    pAction->SetPoint(aPoint);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::MaskScaleHandler()
{
    auto pAction = new MetaMaskScaleAction();

    VersionCompatRead aCompat(mrStream);
    Bitmap aBmp;
    ReadDIB(aBmp, mrStream, true);
    TypeSerializer aSerializer(mrStream);
    Point aPoint;
    aSerializer.readPoint(aPoint);
    Size aSize;
    aSerializer.readSize(aSize);

    pAction->SetBitmap(aBmp);
    pAction->SetPoint(aPoint);
    pAction->SetSize(aSize);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::MaskScalePartHandler()
{
    auto pAction = new MetaMaskScalePartAction();

    VersionCompatRead aCompat(mrStream);
    Bitmap aBmp;
    ReadDIB(aBmp, mrStream, true);
    Color aColor;
    ReadColor(aColor);
    TypeSerializer aSerializer(mrStream);
    Point aDstPt;
    aSerializer.readPoint(aDstPt);
    Size aDstSz;
    aSerializer.readSize(aDstSz);
    Point aSrcPt;
    aSerializer.readPoint(aSrcPt);
    Size aSrcSz;
    aSerializer.readSize(aSrcSz);

    pAction->SetBitmap(aBmp);
    pAction->SetColor(aColor);
    pAction->SetDestPoint(aDstPt);
    pAction->SetDestSize(aDstSz);
    pAction->SetSrcPoint(aSrcPt);
    pAction->SetSrcSize(aSrcSz);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::GradientHandler()
{
    auto pAction = new MetaGradientAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);

    tools::Rectangle aRect;
    aSerializer.readRectangle(aRect);
    Gradient aGradient;
    aSerializer.readGradient(aGradient);

    pAction->SetRect(aRect);
    pAction->SetGradient(aGradient);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::GradientExHandler()
{
    auto pAction = new MetaGradientExAction();

    VersionCompatRead aCompat(mrStream);
    tools::PolyPolygon aPolyPoly;
    ReadPolyPolygon(mrStream, aPolyPoly);
    TypeSerializer aSerializer(mrStream);
    Gradient aGradient;
    aSerializer.readGradient(aGradient);

    pAction->SetGradient(aGradient);
    pAction->SetPolyPolygon(aPolyPoly);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::HatchHandler()
{
    auto pAction = new MetaHatchAction();

    VersionCompatRead aCompat(mrStream);
    tools::PolyPolygon aPolyPoly;
    ReadPolyPolygon(mrStream, aPolyPoly);
    Hatch aHatch;
    ReadHatch(mrStream, aHatch);

    pAction->SetPolyPolygon(aPolyPoly);
    pAction->SetHatch(aHatch);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::WallpaperHandler()
{
    auto pAction = new MetaWallpaperAction();

    VersionCompatRead aCompat(mrStream);
    Wallpaper aWallpaper;
    ReadWallpaper(mrStream, aWallpaper);

    pAction->SetWallpaper(aWallpaper);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::ClipRegionHandler()
{
    auto pAction = new MetaClipRegionAction();

    VersionCompatRead aCompat(mrStream);
    vcl::Region aRegion;
    ReadRegion(mrStream, aRegion);
    bool aClip;
    mrStream.ReadCharAsBool(aClip);

    pAction->SetRegion(aRegion);
    pAction->SetClipping(aClip);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::ISectRectClipRegionHandler()
{
    auto pAction = new MetaISectRectClipRegionAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);
    tools::Rectangle aRect;
    aSerializer.readRectangle(aRect);

    pAction->SetRect(aRect);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::ISectRegionClipRegionHandler()
{
    auto pAction = new MetaISectRegionClipRegionAction();

    VersionCompatRead aCompat(mrStream);
    vcl::Region aRegion;
    ReadRegion(mrStream, aRegion);
    pAction->SetRegion(aRegion);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::MoveClipRegionHandler()
{
    auto pAction = new MetaMoveClipRegionAction();

    VersionCompatRead aCompat(mrStream);
    sal_Int32 nTmpHM(0), nTmpVM(0);
    mrStream.ReadInt32(nTmpHM).ReadInt32(nTmpVM);

    pAction->SetHorzMove(nTmpHM);
    pAction->SetVertMove(nTmpVM);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::TextColorHandler()
{
    auto pAction = new MetaTextColorAction();

    VersionCompatRead aCompat(mrStream);
    Color aColor;
    ReadColor(aColor);

    pAction->SetColor(aColor);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::TextFillColorHandler()
{
    auto pAction = new MetaTextFillColorAction();

    VersionCompatRead aCompat(mrStream);
    Color aColor;
    ReadColor(aColor);
    bool bSet;
    mrStream.ReadCharAsBool(bSet);

    pAction->SetColor(aColor);
    pAction->SetSetting(bSet);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::TextLineColorHandler()
{
    auto pAction = new MetaTextLineColorAction();

    VersionCompatRead aCompat(mrStream);
    Color aColor;
    ReadColor(aColor);
    bool bSet;
    mrStream.ReadCharAsBool(bSet);

    pAction->SetColor(aColor);
    pAction->SetSetting(bSet);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::OverlineColorHandler()
{
    auto pAction = new MetaOverlineColorAction();

    VersionCompatRead aCompat(mrStream);
    Color aColor;
    ReadColor(aColor);
    bool bSet;
    mrStream.ReadCharAsBool(bSet);

    pAction->SetColor(aColor);
    pAction->SetSetting(bSet);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::TextAlignHandler()
{
    auto pAction = new MetaTextAlignAction();

    sal_uInt16 nTmp16(0);

    VersionCompatRead aCompat(mrStream);
    mrStream.ReadUInt16(nTmp16);

    pAction->SetTextAlign(static_cast<TextAlign>(nTmp16));

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::MapModeHandler()
{
    auto pAction = new MetaMapModeAction();

    VersionCompatRead aCompat(mrStream);
    TypeSerializer aSerializer(mrStream);
    MapMode aMapMode;
    aSerializer.readMapMode(aMapMode);

    pAction->SetMapMode(aMapMode);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::FontHandler(ImplMetaReadData* pData)
{
    auto pAction = new MetaFontAction();

    VersionCompatRead aCompat(mrStream);
    vcl::Font aFont;
    ReadFont(mrStream, aFont);
    pData->meActualCharSet = aFont.GetCharSet();
    if (pData->meActualCharSet == RTL_TEXTENCODING_DONTKNOW)
        pData->meActualCharSet = osl_getThreadTextEncoding();

    pAction->SetFont(aFont);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::PushHandler()
{
    auto pAction = new MetaPushAction();

    VersionCompatRead aCompat(mrStream);
    sal_uInt16 tmp;
    mrStream.ReadUInt16(tmp);

    pAction->SetPushFlags(static_cast<PushFlags>(tmp));

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::PopHandler()
{
    auto pAction = new MetaPopAction();

    VersionCompatRead aCompat(mrStream);

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::RasterOpHandler()
{
    auto pAction = new MetaRasterOpAction();

    sal_uInt16 nTmp16(0);

    VersionCompatRead aCompat(mrStream);
    mrStream.ReadUInt16(nTmp16);

    pAction->SetRasterOp(static_cast<RasterOp>(nTmp16));

    return pAction;
}

rtl::Reference<MetaAction> SvmReader::TransparentHandler()
{
    auto pAction = new MetaTransparentAction();

    VersionCompatRead aCompat(mrStream);
    tools::PolyPolygon aPolyPoly;
    ReadPolyPolygon(mrStream, aPolyPoly);
    sal_uInt16 nTransPercent;
    mrStream.ReadUInt16(nTransPercent);

    pAction->SetPolyPolygon(aPolyPoly);
    pAction->SetTransparence(nTransPercent);

    return pAction;
}
/* vim:set shiftwidth=4 softtabstop=4 expandtab: */

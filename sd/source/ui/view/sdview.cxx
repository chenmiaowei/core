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

#include <com/sun/star/embed/NoVisualAreaSizeException.hpp>
#include <com/sun/star/embed/XEmbeddedObject.hpp>
#include <com/sun/star/linguistic2/XSpellChecker1.hpp>

#include <View.hxx>
#include <editeng/outlobj.hxx>
#include <editeng/unolingu.hxx>
#include <o3tl/deleter.hxx>
#include <svx/obj3d.hxx>
#include <svx/fmview.hxx>
#include <editeng/outliner.hxx>
#include <svx/svdograf.hxx>
#include <svx/svdoole2.hxx>
#include <svx/svdundo.hxx>

#include <vcl/settings.hxx>

#include <sfx2/dispatch.hxx>
#include <svx/svdpagv.hxx>
#include <svx/svdoutl.hxx>
#include <svx/sdr/contact/displayinfo.hxx>

#include <svx/svdetc.hxx>
#include <editeng/editstat.hxx>

#include <sfx2/viewfrm.hxx>
#include <toolkit/helper/vclunohelper.hxx>
#include <svx/xfillit0.hxx>

#include <app.hrc>
#include <strings.hrc>
#include <Window.hxx>
#include <Client.hxx>
#include <drawdoc.hxx>
#include <DrawDocShell.hxx>
#include <sdmod.hxx>
#include <sdpage.hxx>
#include <sdresid.hxx>
#include <unokywds.hxx>
#include <ViewClipboard.hxx>
#include <undo/undomanager.hxx>
#include <svx/sdr/contact/viewobjectcontact.hxx>
#include <svx/sdr/contact/viewcontact.hxx>
#include <svx/svdotable.hxx>
#include <EventMultiplexer.hxx>
#include <ViewShellBase.hxx>
#include <ViewShell.hxx>

#include <basegfx/polygon/b2dpolygontools.hxx>
#include <basegfx/color/bcolor.hxx>
#include <drawinglayer/primitive2d/polygonprimitive2d.hxx>
#include <drawinglayer/primitive2d/textlayoutdevice.hxx>
#include <svx/sdr/contact/objectcontact.hxx>
#include <svx/sdr/table/tablecontroller.hxx>
#include <basegfx/matrix/b2dhommatrix.hxx>
#include <drawinglayer/primitive2d/textprimitive2d.hxx>
#include <svx/unoapi.hxx>
#include <basegfx/matrix/b2dhommatrixtools.hxx>
#include <comphelper/lok.hxx>
#include <sfx2/lokhelper.hxx>
#include <LibreOfficeKit/LibreOfficeKitEnums.h>
#include <DrawController.hxx>

#include <memory>
#include <numeric>

using namespace com::sun::star;
using namespace com::sun::star::uno;
using namespace sdr::table;
namespace sd {

View::View(
    SdDrawDocument& rDrawDoc,
    OutputDevice* pOutDev,
    ViewShell* pViewShell)
:   FmFormView(rDrawDoc, pOutDev),
    mrDoc(rDrawDoc),
    mpDocSh(rDrawDoc.GetDocSh()),
    mpViewSh(pViewShell),
    mpDropMarkerObj(nullptr),
    mnDragSrcPgNum(SDRPAGE_NOTFOUND),
    mnAction(DND_ACTION_NONE),
    maDropErrorIdle("sd View DropError"),
    maDropInsertFileIdle("sd View DropInsertFile"),
    mnLockRedrawSmph(0),
    mbIsDropAllowed(true),
    maSmartTags(*this),
    mpClipboard (new ViewClipboard (*this))
{
    // #i73602# Use default from the configuration
    SetBufferedOverlayAllowed(getOptionsDrawinglayer().IsOverlayBuffer_DrawImpress());

    // #i74769#, #i75172# Use default from the configuration
    SetBufferedOutputAllowed(getOptionsDrawinglayer().IsPaintBuffer_DrawImpress());

    EnableExtendedKeyInputDispatcher(false);
    EnableExtendedMouseEventDispatcher(false);

    SetUseIncompatiblePathCreateInterface(false);

    SetMinMoveDistancePixel(2);
    SetHitTolerancePixel(2);
    SetMeasureLayer(sUNO_LayerName_measurelines);

    // Timer for delayed drop (has to be for MAC)
    maDropErrorIdle.SetInvokeHandler( LINK(this, View, DropErrorHdl) );
    maDropInsertFileIdle.SetInvokeHandler( LINK(this, View, DropInsertFileHdl) );
}

void View::ImplClearDrawDropMarker()
{
    mpDropMarker.reset();
}

View::~View()
{
    maSmartTags.Dispose();

    // release content of selection clipboard, if we own the content
    ClearSelectionClipboard();

    maDropErrorIdle.Stop();
    maDropInsertFileIdle.Stop();

    ImplClearDrawDropMarker();

    while(PaintWindowCount())
    {
        // remove all registered OutDevs
        // cid#1485150 silence Uncaught exception
        suppress_fun_call_w_exception(DeleteWindowFromPaintView(GetFirstOutputDevice()));
    }
}

namespace {

class ViewRedirector : public sdr::contact::ViewObjectContactRedirector
{
public:
    ViewRedirector();

    // all default implementations just call the same methods at the original. To do something
    // different, override the method and at least do what the method does.
    virtual drawinglayer::primitive2d::Primitive2DContainer createRedirectedPrimitive2DSequence(
        const sdr::contact::ViewObjectContact& rOriginal,
        const sdr::contact::DisplayInfo& rDisplayInfo) override;
};

}

ViewRedirector::ViewRedirector()
{
}

drawinglayer::primitive2d::Primitive2DContainer ViewRedirector::createRedirectedPrimitive2DSequence(
    const sdr::contact::ViewObjectContact& rOriginal,
    const sdr::contact::DisplayInfo& rDisplayInfo)
{
    SdrObject* pObject = rOriginal.GetViewContact().TryToGetSdrObject();
    drawinglayer::primitive2d::Primitive2DContainer xRetval;

    if(pObject && pObject->getSdrPageFromSdrObject())
    {
        const bool bDoCreateGeometry(pObject->getSdrPageFromSdrObject()->checkVisibility( rOriginal, rDisplayInfo, true ));

        if(!bDoCreateGeometry &&
            (( pObject->GetObjInventor() != SdrInventor::Default ) || ( pObject->GetObjIdentifier() != OBJ_PAGE )) )
            return xRetval;

        PresObjKind eKind(PresObjKind::NONE);
        const bool bSubContentProcessing(rDisplayInfo.GetSubContentActive());
        const bool bIsMasterPageObject(pObject->getSdrPageFromSdrObject()->IsMasterPage());
        const bool bIsPrinting(rOriginal.GetObjectContact().isOutputToPrinter());
        const SdrPageView* pPageView = rOriginal.GetObjectContact().TryToGetSdrPageView();
        const SdrPage* pVisualizedPage = GetSdrPageFromXDrawPage(rOriginal.GetObjectContact().getViewInformation2D().getVisualizedPage());
        const SdPage* pObjectsSdPage = dynamic_cast< SdPage* >(pObject->getSdrPageFromSdrObject());
        const bool bIsInsidePageObj(pPageView && pPageView->GetPage() != pVisualizedPage);

        // check if we need to draw a placeholder border. Never do it for
        // objects inside a SdrPageObj and never when printing
        if(!bIsInsidePageObj && !bIsPrinting)
        {
            bool bCreateOutline(false);

            if( pObject->IsEmptyPresObj() && dynamic_cast< SdrTextObj *>( pObject ) !=  nullptr )
            {
                if( !bSubContentProcessing || !pObject->IsNotVisibleAsMaster() )
                {
                    eKind = pObjectsSdPage ? pObjectsSdPage->GetPresObjKind(pObject) : PresObjKind::NONE;
                    bCreateOutline = true;
                }
            }
            else if( ( pObject->GetObjInventor() == SdrInventor::Default ) && ( pObject->GetObjIdentifier() == OBJ_TEXT ) )
            {
                if( pObjectsSdPage )
                {
                    eKind = pObjectsSdPage->GetPresObjKind(pObject);

                    if((eKind == PresObjKind::Footer) || (eKind == PresObjKind::Header) || (eKind == PresObjKind::DateTime) || (eKind == PresObjKind::SlideNumber) )
                    {
                        if( !bSubContentProcessing )
                        {
                            // only draw a boundary for header&footer objects on the masterpage itself
                            bCreateOutline = true;
                        }
                    }
                }
            }
            else if( ( pObject->GetObjInventor() == SdrInventor::Default ) && ( pObject->GetObjIdentifier() == OBJ_PAGE ) )
            {
                // only for handout page, else this frame will be created for each
                // page preview object in SlideSorter and PagePane
                if(pObjectsSdPage && PageKind::Handout == pObjectsSdPage->GetPageKind())
                {
                    bCreateOutline = true;
                }
            }

            if(bCreateOutline)
            {
                // empty presentation objects get a gray frame
                const svtools::ColorConfig aColorConfig;
                const svtools::ColorConfigValue aColor( aColorConfig.GetColorValue( svtools::OBJECTBOUNDARIES ) );

                if( aColor.bIsVisible )
                {
                    // get basic object transformation
                    const basegfx::BColor aRGBColor(aColor.nColor.getBColor());
                    basegfx::B2DHomMatrix aObjectMatrix;
                    basegfx::B2DPolyPolygon aObjectPolyPolygon;
                    pObject->TRGetBaseGeometry(aObjectMatrix, aObjectPolyPolygon);

                    // create dashed border
                    {
                        // create object polygon
                        basegfx::B2DPolygon aPolygon(basegfx::utils::createUnitPolygon());
                        aPolygon.transform(aObjectMatrix);

                        // create line and stroke attribute
                        ::std::vector< double > aDotDashArray;

                        aDotDashArray.push_back(160.0);
                        aDotDashArray.push_back(80.0);

                        const double fFullDotDashLen(::std::accumulate(aDotDashArray.begin(), aDotDashArray.end(), 0.0));
                        const drawinglayer::attribute::LineAttribute aLine(aRGBColor);
                        const drawinglayer::attribute::StrokeAttribute aStroke(aDotDashArray, fFullDotDashLen);

                        // create primitive and add
                        const drawinglayer::primitive2d::Primitive2DReference xRef(new drawinglayer::primitive2d::PolygonStrokePrimitive2D(
                            aPolygon,
                            aLine,
                            aStroke));
                        xRetval.push_back(xRef);
                    }

                    // now paint the placeholder description, but only when masterpage
                    // is displayed as page directly (MasterPage view)
                    if(!bSubContentProcessing && bIsMasterPageObject)
                    {
                        OUString aObjectString;

                        switch( eKind )
                        {
                            case PresObjKind::Title:
                            {
                                if(pObjectsSdPage && pObjectsSdPage->GetPageKind() == PageKind::Standard)
                                {
                                    static OUString aTitleAreaStr(SdResId(STR_PLACEHOLDER_DESCRIPTION_TITLE));
                                    aObjectString = aTitleAreaStr;
                                }

                                break;
                            }
                            case PresObjKind::Outline:
                            {
                                static OUString aOutlineAreaStr(SdResId(STR_PLACEHOLDER_DESCRIPTION_OUTLINE));
                                aObjectString = aOutlineAreaStr;
                                break;
                            }
                            case PresObjKind::Footer:
                            {
                                static OUString aFooterAreaStr(SdResId(STR_PLACEHOLDER_DESCRIPTION_FOOTER));
                                aObjectString = aFooterAreaStr;
                                break;
                            }
                            case PresObjKind::Header:
                            {
                                static OUString aHeaderAreaStr(SdResId(STR_PLACEHOLDER_DESCRIPTION_HEADER));
                                aObjectString = aHeaderAreaStr;
                                break;
                            }
                            case PresObjKind::DateTime:
                            {
                                static OUString aDateTimeStr(SdResId(STR_PLACEHOLDER_DESCRIPTION_DATETIME));
                                aObjectString = aDateTimeStr;
                                break;
                            }
                            case PresObjKind::Notes:
                            {
                                static OUString aDateTimeStr(SdResId(STR_PLACEHOLDER_DESCRIPTION_NOTES));
                                aObjectString = aDateTimeStr;
                                break;
                            }
                            case PresObjKind::SlideNumber:
                            {
                                if(pObjectsSdPage && pObjectsSdPage->GetPageKind() == PageKind::Standard)
                                {
                                    static OUString aSlideAreaStr(SdResId(STR_PLACEHOLDER_DESCRIPTION_SLIDE));
                                    aObjectString = aSlideAreaStr;
                                }
                                else
                                {
                                    static OUString aNumberAreaStr(SdResId(STR_PLACEHOLDER_DESCRIPTION_NUMBER));
                                    aObjectString = aNumberAreaStr;
                                }
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }

                        if( !aObjectString.isEmpty() )
                        {
                            // decompose object matrix to be able to place text correctly
                            basegfx::B2DTuple aScale;
                            basegfx::B2DTuple aTranslate;
                            double fRotate, fShearX;
                            aObjectMatrix.decompose(aScale, aTranslate, fRotate, fShearX);

                            // create font
                            SdrTextObj* pTextObj = dynamic_cast< SdrTextObj* >( pObject );
                            const SdrTextVertAdjust eTVA(pTextObj ? pTextObj->GetTextVerticalAdjust() : SDRTEXTVERTADJUST_CENTER);
                            vcl::Font aScaledVclFont;

                            // use a text size factor to get more reliable text sizes from the text layouter
                            // (and from vcl), tipp from HDU
                            static const sal_uInt32 nTextSizeFactor(100);

                            // use a factor to get more linear text size calculations
                            aScaledVclFont.SetFontHeight( 500 * nTextSizeFactor );

                            // get basic geometry and get text size
                            drawinglayer::primitive2d::TextLayouterDevice aTextLayouter;
                            aTextLayouter.setFont(aScaledVclFont);
                            const sal_Int32 nTextLength(aObjectString.getLength());

                            // do not forget to use the factor again to get the width for the 500
                            const double fTextWidth(aTextLayouter.getTextWidth(aObjectString, 0, nTextLength) * (1.0 / nTextSizeFactor));
                            const double fTextHeight(aTextLayouter.getTextHeight() * (1.0 / nTextSizeFactor));

                            // calculate text primitive position. If text is at bottom, use top for
                            // the extra text and vice versa
                            const double fHorDist(125);
                            const double fVerDist(125);
                            const double fPosX((aTranslate.getX() + aScale.getX()) - fTextWidth - fHorDist);
                            const double fPosY((SDRTEXTVERTADJUST_BOTTOM == eTVA)
                                ? aTranslate.getY() - fVerDist + fTextHeight
                                : (aTranslate.getY() + aScale.getY()) - fVerDist);

                            // get font attributes; use normally scaled font
                            vcl::Font aVclFont;
                            basegfx::B2DVector aTextSizeAttribute;

                            aVclFont.SetFontHeight( 500 );

                            const drawinglayer::attribute::FontAttribute aFontAttribute(
                                drawinglayer::primitive2d::getFontAttributeFromVclFont(
                                    aTextSizeAttribute,
                                    aVclFont,
                                    false,
                                    false));

                            // fill text matrix
                            const basegfx::B2DHomMatrix aTextMatrix(basegfx::utils::createScaleShearXRotateTranslateB2DHomMatrix(
                                aTextSizeAttribute.getX(), aTextSizeAttribute.getY(),
                                fShearX,
                                fRotate,
                                fPosX, fPosY));

                            // create DXTextArray (can be empty one)
                            const ::std::vector< double > aDXArray{};

                            // create locale; this may need some more information in the future
                            const css::lang::Locale aLocale;

                            // create primitive and add
                            const drawinglayer::primitive2d::Primitive2DReference xRef(
                                new drawinglayer::primitive2d::TextSimplePortionPrimitive2D(
                                    aTextMatrix,
                                    aObjectString,
                                    0,
                                    nTextLength,
                                    aDXArray,
                                    aFontAttribute,
                                    aLocale,
                                    aRGBColor));
                            xRetval.push_back(xRef);
                        }
                    }
                }
            }
        }

        if(bDoCreateGeometry)
        {
            xRetval.append(
                sdr::contact::ViewObjectContactRedirector::createRedirectedPrimitive2DSequence(
                    rOriginal,
                    rDisplayInfo));
        }
    }
    else
    {
        // not a SdrObject visualisation (maybe e.g. page) or no page
        xRetval = sdr::contact::ViewObjectContactRedirector::createRedirectedPrimitive2DSequence(rOriginal, rDisplayInfo);
    }

    return xRetval;
}

/**
 * The event will be forwarded to the View
 */
void View::CompleteRedraw(OutputDevice* pOutDev, const vcl::Region& rReg, sdr::contact::ViewObjectContactRedirector* pRedirector /*=0*/)
{
    // execute ??
    if (mnLockRedrawSmph != 0)
        return;

    SdrPageView* pPgView = GetSdrPageView();

    if (pPgView)
    {
        SdPage* pPage = static_cast<SdPage*>( pPgView->GetPage() );
        if( pPage )
        {
            SdrOutliner& rOutl = mrDoc.GetDrawOutliner();
            bool bScreenDisplay(true);

            // #i75566# printing; suppress AutoColor BackgroundColor generation
            // for visibility reasons by giving GetPageBackgroundColor()
            // the needed hint
            // #i75566# PDF export; suppress AutoColor BackgroundColor generation (see printing)
            if (pOutDev && ((OUTDEV_PRINTER == pOutDev->GetOutDevType())
                    || (OUTDEV_PDF == pOutDev->GetOutDevType())))
                bScreenDisplay = false;

            // #i75566# Name change GetBackgroundColor -> GetPageBackgroundColor and
            // hint value if screen display. Only then the AutoColor mechanisms shall be applied
            rOutl.SetBackgroundColor( pPage->GetPageBackgroundColor(pPgView, bScreenDisplay) );
        }
    }

    ViewRedirector aViewRedirector;
    FmFormView::CompleteRedraw(pOutDev, rReg, pRedirector ? pRedirector : &aViewRedirector);
}

void View::MarkListHasChanged()
{
    FmFormView::MarkListHasChanged();

    if( GetMarkedObjectCount() > 0 )
        maSmartTags.deselect();
}

bool View::SetAttributes(const SfxItemSet& rSet, bool bReplaceAll, bool /*bSlide*/, bool /*bMaster*/)
{
    bool bOk = FmFormView::SetAttributes(rSet, bReplaceAll);
    return bOk;
}

void View::GetAttributes( SfxItemSet& rTargetSet, bool bOnlyHardAttr ) const
{
    FmFormView::GetAttributes( rTargetSet, bOnlyHardAttr );
}

/**
 * Is a presentation object selected?
 */
bool View::IsPresObjSelected(bool bOnPage, bool bOnMasterPage, bool bCheckPresObjListOnly, bool bCheckLayoutOnly) const
{
    SdrMarkList* pMarkList;

    if (mnDragSrcPgNum != SDRPAGE_NOTFOUND &&
        mnDragSrcPgNum != GetSdrPageView()->GetPage()->GetPageNum())
    {
        /* Drag&Drop is in progress
           Source and destination page are different:
           we use the saved mark list */
        pMarkList = mpDragSrcMarkList.get();
    }
    else
    {
        // We use the current mark list
        pMarkList = new SdrMarkList(GetMarkedObjectList());
    }

    SdrMark* pMark;
    SdPage* pPage;

    bool bSelected = false;
    bool bMasterPage = false;

    for (size_t nMark = pMarkList->GetMarkCount(); nMark && !bSelected; )
    {
        --nMark;
        // Backwards through mark list
        pMark = pMarkList->GetMark(nMark);
        SdrObject* pObj = pMark->GetMarkedSdrObj();

        if ( pObj && ( bCheckPresObjListOnly || pObj->IsEmptyPresObj() || pObj->GetUserCall() ) )
        {
            pPage = static_cast<SdPage*>( pObj->getSdrPageFromSdrObject() );
            bMasterPage = pPage && pPage->IsMasterPage();

            if ( (bMasterPage && bOnMasterPage) || (!bMasterPage && bOnPage) )
            {
                if ( pPage && pPage->IsPresObj(pObj) )
                {
                    if( bCheckLayoutOnly )
                    {
                        PresObjKind eKind = pPage->GetPresObjKind(pObj);

                        if((eKind != PresObjKind::Footer) && (eKind != PresObjKind::Header) && (eKind != PresObjKind::DateTime) && (eKind != PresObjKind::SlideNumber) )
                            bSelected = true;
                    }
                    else
                    {
                        bSelected = true;
                    }
                }
            }
        }
    }

    if (pMarkList != mpDragSrcMarkList.get())
    {
       delete pMarkList;
    }

    return bSelected;
}

void View::SelectAll()
{
    if ( IsTextEdit() )
    {
        OutlinerView* pOLV = GetTextEditOutlinerView();
        const ::Outliner* pOutliner = GetTextEditOutliner();
        pOLV->SelectRange( 0, pOutliner->GetParagraphCount() );
    }
    else
    {
        MarkAll();
    }
}

bool View::SetStyleSheet(SfxStyleSheet* pStyleSheet, bool bDontRemoveHardAttr)
{
    // forward to SdrView
    FmFormView::SetStyleSheet(pStyleSheet, bDontRemoveHardAttr);
    return true;
}

/**
 * Start text input
 */
static void SetSpellOptions( const SdDrawDocument& rDoc, EEControlBits& rCntrl )
{
    bool bOnlineSpell = rDoc.GetOnlineSpell();

    if( bOnlineSpell )
        rCntrl |= EEControlBits::ONLINESPELLING;
    else
        rCntrl &= ~EEControlBits::ONLINESPELLING;
}

void OutlinerMasterViewFilter::Start(SdrOutliner *pOutl)
{
    m_pOutl = pOutl;
    OutlinerView* pOutlView = m_pOutl->GetView(0);
    m_bReadOnly = pOutlView->IsReadOnly();
    pOutlView->SetReadOnly(true);
}

void OutlinerMasterViewFilter::End()
{
    if (m_pOutl)
    {
        OutlinerView* pOutlView = m_pOutl->GetView(0);
        pOutlView->SetReadOnly(m_bReadOnly);
        m_pOutl = nullptr;
    }
}

SfxViewShell* View::GetSfxViewShell() const
{
    SfxViewShell* pRet = nullptr;

    if (mpViewSh)
        pRet = &mpViewSh->GetViewShellBase();

    return pRet;
}

bool View::SdrBeginTextEdit(
    SdrObject* pObj, SdrPageView* pPV, vcl::Window* pWin,
    bool bIsNewObj,
    SdrOutliner* pOutl, OutlinerView* pGivenOutlinerView,
    bool bDontDeleteOutliner, bool bOnlyOneView, bool bGrabFocus )
{
    SdrPage* pPage = pObj ? pObj->getSdrPageFromSdrObject() : nullptr;
    bool bMasterPage = pPage && pPage->IsMasterPage();

    GetViewShell()->GetViewShellBase().GetEventMultiplexer()->MultiplexEvent(
        EventMultiplexerEventId::BeginTextEdit, static_cast<void*>(pObj) );

    if( pOutl==nullptr && pObj )
        pOutl = SdrMakeOutliner(OutlinerMode::TextObject, pObj->getSdrModelFromSdrObject()).release();

    // make draw&impress specific initialisations
    if( pOutl )
    {
        pOutl->SetStyleSheetPool(static_cast<SfxStyleSheetPool*>( mrDoc.GetStyleSheetPool() ));
        pOutl->SetCalcFieldValueHdl(LINK(SD_MOD(), SdModule, CalcFieldValueHdl));
        EEControlBits nCntrl = pOutl->GetControlWord();
        nCntrl |= EEControlBits::ALLOWBIGOBJS;
        nCntrl |= EEControlBits::MARKFIELDS;
        nCntrl |= EEControlBits::AUTOCORRECT;

        nCntrl &= ~EEControlBits::ULSPACESUMMATION;
        if ( mrDoc.IsSummationOfParagraphs() )
            nCntrl |= EEControlBits::ULSPACESUMMATION;

        SetSpellOptions( mrDoc, nCntrl );

        pOutl->SetControlWord(nCntrl);

        Reference< linguistic2::XSpellChecker1 > xSpellChecker( LinguMgr::GetSpellChecker() );
        if ( xSpellChecker.is() )
            pOutl->SetSpeller( xSpellChecker );

        Reference< linguistic2::XHyphenator > xHyphenator( LinguMgr::GetHyphenator() );
        if( xHyphenator.is() )
            pOutl->SetHyphenator( xHyphenator );

        pOutl->SetDefaultLanguage( Application::GetSettings().GetLanguageTag().getLanguageType() );
    }

    bool bReturn = FmFormView::SdrBeginTextEdit(
        pObj, pPV, pWin, bIsNewObj, pOutl,
        pGivenOutlinerView, bDontDeleteOutliner,
        bOnlyOneView, bGrabFocus);

    if ( mpViewSh )
    {
        mpViewSh->GetViewShellBase().GetDrawController().FireSelectionChangeListener();

        if (pObj && pObj->GetObjIdentifier() == OBJ_TABLE)
            mpViewSh->UpdateScrollBars();

        if (comphelper::LibreOfficeKit::isActive())
        {
            if (OutlinerView* pView = GetTextEditOutlinerView())
            {
                ::tools::Rectangle aRectangle = pView->GetOutputArea();
                if (pWin && pWin->GetMapMode().GetMapUnit() == MapUnit::Map100thMM)
                    aRectangle = OutputDevice::LogicToLogic(aRectangle, MapMode(MapUnit::Map100thMM), MapMode(MapUnit::MapTwip));
                OString sRectangle = aRectangle.toString();
                SfxLokHelper::notifyOtherViews(&mpViewSh->GetViewShellBase(), LOK_CALLBACK_VIEW_LOCK, "rectangle", sRectangle);
            }
        }
    }

    if (::Outliner* pOL = bReturn ? GetTextEditOutliner() : nullptr)
    {
        if (pObj)
        {
            if( pObj->GetObjInventor() == SdrInventor::Default && pObj->GetObjIdentifier() == OBJ_TABLE )
            {
                Color aBackground = GetTextEditBackgroundColor(*this);
                pOL->SetBackgroundColor( aBackground  );
            }
            else
            {
                pObj->setSuitableOutlinerBg(*pOL);
            }
        }

        pOL->SetParaInsertedHdl(LINK(this, View, OnParagraphInsertedHdl));
        pOL->SetParaRemovingHdl(LINK(this, View, OnParagraphRemovingHdl));
    }

    if (bMasterPage && bReturn && pOutl)
    {
        const SdrTextObj* pTextObj = pOutl->GetTextObj();
        const SdPage* pSdPage = pTextObj ? static_cast<const SdPage*>(pTextObj->getSdrPageFromSdrObject()) : nullptr;
        const PresObjKind eKind = pSdPage ? pSdPage->GetPresObjKind(const_cast<SdrTextObj*>(pTextObj)) : PresObjKind::NONE;
        switch (eKind)
        {
            case PresObjKind::Title:
            case PresObjKind::Outline:
            case PresObjKind::Text:
                maMasterViewFilter.Start(pOutl);
                break;
            default:
                break;
        }
    }

    return bReturn;
}

/** ends current text editing */
SdrEndTextEditKind View::SdrEndTextEdit(bool bDontDeleteReally)
{
    maMasterViewFilter.End();

    ::tools::WeakReference<SdrTextObj> xObj( GetTextEditObject() );

    bool bDefaultTextRestored = RestoreDefaultText( xObj.get() );

    SdrEndTextEditKind eKind = FmFormView::SdrEndTextEdit(bDontDeleteReally);

    if( bDefaultTextRestored )
    {
        if( xObj.is() && !xObj->IsEmptyPresObj() )
        {
            xObj->SetEmptyPresObj( true );
        }
        else
        {
            eKind = SdrEndTextEditKind::Unchanged;
        }
    }
    else if( xObj.is() && xObj->IsEmptyPresObj() )
    {
        SdrTextObj* pObj = xObj.get();
        if( pObj && pObj->HasText() )
        {
            SdrPage* pPage = pObj->getSdrPageFromSdrObject();
            if( !pPage || !pPage->IsMasterPage() )
                pObj->SetEmptyPresObj( false );
        }
    }

    GetViewShell()->GetViewShellBase().GetEventMultiplexer()->MultiplexEvent(
        EventMultiplexerEventId::EndTextEdit,
        static_cast<void*>(xObj.get()) );

    if( xObj.is() )
    {
        if ( mpViewSh )
        {
            mpViewSh->GetViewShellBase().GetDrawController().FireSelectionChangeListener();

            if (comphelper::LibreOfficeKit::isActive())
                SfxLokHelper::notifyOtherViews(&mpViewSh->GetViewShellBase(), LOK_CALLBACK_VIEW_LOCK, "rectangle", "EMPTY");

        }

        SdPage* pPage = dynamic_cast< SdPage* >( xObj->getSdrPageFromSdrObject() );
        if( pPage )
            pPage->onEndTextEdit( xObj.get() );
    }

    return eKind;
}

/** restores the default text if the given text object is currently in edit mode and
    no text has been entered already. Is only useful just before text edit ends. */
bool View::RestoreDefaultText( SdrTextObj* pTextObj )
{
    bool bRestored = false;

    if( pTextObj && (pTextObj == GetTextEditObject()) )
    {
        if( !pTextObj->HasText() )
        {
            SdPage* pPage = dynamic_cast< SdPage* >( pTextObj->getSdrPageFromSdrObject() );

            if(pPage)
            {
                bRestored = pPage->RestoreDefaultText( pTextObj );
                if( bRestored )
                {
                    SdrOutliner* pOutliner = GetTextEditOutliner();
                    pTextObj->SetTextEditOutliner( pOutliner );
                    OutlinerParaObject* pParaObj = pTextObj->GetOutlinerParaObject();
                    if (pOutliner)
                        pOutliner->SetText(*pParaObj);
                }
            }
        }
    }

    return bRestored;
}

/**
 * Sets the original size of the marked objects.
 */
void View::SetMarkedOriginalSize()
{
    std::unique_ptr<SdrUndoGroup> pUndoGroup(new SdrUndoGroup(mrDoc));
    const size_t nCount = GetMarkedObjectCount();
    bool            bOK = false;

    for( size_t i = 0; i < nCount; ++i )
    {
        SdrObject* pObj = GetMarkedObjectByIndex(i);

        if( pObj->GetObjInventor() == SdrInventor::Default )
        {
            if( pObj->GetObjIdentifier() == OBJ_OLE2 )
            {
                uno::Reference < embed::XEmbeddedObject > xObj = static_cast<SdrOle2Obj*>(pObj)->GetObjRef();
                if( xObj.is() )
                {
                    // TODO/LEAN: working with VisualArea can switch object to running state

                    sal_Int64 nAspect = static_cast<SdrOle2Obj*>(pObj)->GetAspect();
                    Size aOleSize;

                    if ( nAspect == embed::Aspects::MSOLE_ICON )
                    {
                        MapMode aMap100( MapUnit::Map100thMM );
                        aOleSize = static_cast<SdrOle2Obj*>(pObj)->GetOrigObjSize( &aMap100 );
                        bOK = true;
                    }
                    else
                    {
                        MapUnit aUnit = VCLUnoHelper::UnoEmbed2VCLMapUnit( xObj->getMapUnit( nAspect ) );
                        try
                        {
                            awt::Size aSz = xObj->getVisualAreaSize( nAspect );
                            aOleSize = OutputDevice::LogicToLogic(Size(aSz.Width, aSz.Height), MapMode(aUnit), MapMode(MapUnit::Map100thMM));
                            bOK = true;
                        }
                        catch( embed::NoVisualAreaSizeException& )
                        {}
                    }

                    if ( bOK )
                    {
                        ::tools::Rectangle   aDrawRect( pObj->GetLogicRect() );

                        pUndoGroup->AddAction( mrDoc.GetSdrUndoFactory().CreateUndoGeoObject( *pObj ) );
                        pObj->Resize( aDrawRect.TopLeft(), Fraction( aOleSize.Width(), aDrawRect.GetWidth() ),
                                                           Fraction( aOleSize.Height(), aDrawRect.GetHeight() ) );
                    }
                }
            }
            else if( pObj->GetObjIdentifier() == OBJ_GRAF )
            {
                const SdrGrafObj* pSdrGrafObj = static_cast< const SdrGrafObj* >(pObj);
                const Size aSize = pSdrGrafObj->getOriginalSize( );
                pUndoGroup->AddAction( GetModel()->GetSdrUndoFactory().CreateUndoGeoObject(*pObj ) );
                ::tools::Rectangle aRect( pObj->GetLogicRect() );
                aRect.SetSize( aSize );
                pObj->SetLogicRect( aRect );
                bOK = true;
            }
        }
    }

    if( bOK )
    {
        pUndoGroup->SetComment(SdResId(STR_UNDO_ORIGINALSIZE));
        mpDocSh->GetUndoManager()->AddUndoAction(std::move(pUndoGroup));
    }
}

/**
 * Connect OLE object to client.
 */
void View::DoConnect(SdrOle2Obj* pObj)
{
    if (!mpViewSh)
        return;

    uno::Reference < embed::XEmbeddedObject > xObj( pObj->GetObjRef() );
    if( !xObj.is() )
        return;

    ::sd::Window* pWindow = mpViewSh->GetActiveWindow();
    SfxInPlaceClient* pSdClient = mpViewSh-> GetViewShellBase().FindIPClient( xObj, pWindow );
    if ( pSdClient )
        return;

    pSdClient = new Client(pObj, mpViewSh, pWindow);
    ::tools::Rectangle aRect = pObj->GetLogicRect();
    {
        // TODO/LEAN: working with visual area can switch object to running state
        Size aDrawSize = aRect.GetSize();

        MapMode aMapMode( mrDoc.GetScaleUnit() );
        Size aObjAreaSize = pObj->GetOrigObjSize( &aMapMode );

        Fraction aScaleWidth (aDrawSize.Width(),  aObjAreaSize.Width() );
        Fraction aScaleHeight(aDrawSize.Height(), aObjAreaSize.Height() );
        aScaleWidth.ReduceInaccurate(10);       // compatible to SdrOle2Obj
        aScaleHeight.ReduceInaccurate(10);
        pSdClient->SetSizeScale(aScaleWidth, aScaleHeight);

        // visible area is only changed in-place!
        // the object area must be set after the scaling, since it triggers resize
        aRect.SetSize(aObjAreaSize);
        pSdClient->SetObjArea(aRect);
    }
}

bool View::IsMorphingAllowed() const
{
    const SdrMarkList&  rMarkList = GetMarkedObjectList();
    bool                bRet = false;

    if ( rMarkList.GetMarkCount() == 2 )
    {
        const SdrObject*    pObj1 = rMarkList.GetMark( 0 )->GetMarkedSdrObj();
        const SdrObject*    pObj2 = rMarkList.GetMark( 1 )->GetMarkedSdrObj();
        const sal_uInt16        nKind1 = pObj1->GetObjIdentifier();
        const sal_uInt16        nKind2 = pObj2->GetObjIdentifier();

        if ( ( nKind1 != OBJ_TEXT && nKind2 != OBJ_TEXT ) &&
             ( nKind1 != OBJ_TITLETEXT && nKind2 != OBJ_TITLETEXT ) &&
             ( nKind1 != OBJ_OUTLINETEXT && nKind2 != OBJ_OUTLINETEXT ) &&
             ( nKind1 != OBJ_GRUP && nKind2 != OBJ_GRUP ) &&
             ( nKind1 != OBJ_LINE && nKind2 != OBJ_LINE ) &&
             ( nKind1 != OBJ_PLIN && nKind2 != OBJ_PLIN ) &&
             ( nKind1 != OBJ_PATHLINE && nKind2 != OBJ_PATHLINE ) &&
             ( nKind1 != OBJ_FREELINE && nKind2 != OBJ_FREELINE ) &&
             ( nKind1 != OBJ_PATHPLIN && nKind2 != OBJ_PATHPLIN ) &&
             ( nKind1 != OBJ_MEASURE && nKind2 != OBJ_MEASURE ) &&
             ( nKind1 != OBJ_EDGE && nKind2 != OBJ_EDGE ) &&
             ( nKind1 != OBJ_GRAF && nKind2 != OBJ_GRAF ) &&
             ( nKind1 != OBJ_OLE2 && nKind2 != OBJ_OLE2 ) &&
             ( nKind1 != OBJ_CAPTION && nKind2 !=  OBJ_CAPTION ) &&
             dynamic_cast< const E3dObject *>( pObj1 ) == nullptr && dynamic_cast< const E3dObject *>( pObj2 ) ==  nullptr )
        {
            SfxItemSet      aSet1( mrDoc.GetPool(), svl::Items<XATTR_FILLSTYLE, XATTR_FILLSTYLE> );
            SfxItemSet      aSet2( mrDoc.GetPool(), svl::Items<XATTR_FILLSTYLE, XATTR_FILLSTYLE> );

            aSet1.Put(pObj1->GetMergedItemSet());
            aSet2.Put(pObj2->GetMergedItemSet());

            const drawing::FillStyle eFillStyle1 = aSet1.Get( XATTR_FILLSTYLE ).GetValue();
            const drawing::FillStyle eFillStyle2 = aSet2.Get( XATTR_FILLSTYLE ).GetValue();

            if( ( eFillStyle1 == drawing::FillStyle_NONE || eFillStyle1 == drawing::FillStyle_SOLID ) &&
                ( eFillStyle2 == drawing::FillStyle_NONE || eFillStyle2 == drawing::FillStyle_SOLID ) )
                bRet = true;
        }
    }

    return bRet;
}

bool View::IsVectorizeAllowed() const
{
    const SdrMarkList&  rMarkList = GetMarkedObjectList();
    bool                bRet = false;

    if( rMarkList.GetMarkCount() == 1 )
    {
        const SdrGrafObj* pObj = dynamic_cast< const SdrGrafObj* >(rMarkList.GetMark( 0 )->GetMarkedSdrObj());

        if(pObj)
        {
            if(GraphicType::Bitmap == pObj->GetGraphicType() && !pObj->isEmbeddedVectorGraphicData())
            {
                bRet = true;
            }
        }
    }

    return bRet;
}

void View::onAccessibilityOptionsChanged()
{
    if( !mpViewSh )
        return;

    ::sd::Window* pWindow = mpViewSh->GetActiveWindow();
    if( !pWindow )
        return;

    const StyleSettings& rStyleSettings = pWindow->GetSettings().GetStyleSettings();

    SvtAccessibilityOptions& aAccOptions = getAccessibilityOptions();

    if( mpViewSh->GetViewFrame() && mpViewSh->GetViewFrame()->GetDispatcher() )
    {
        sal_uInt16 nOutputSlot, nPreviewSlot;

        if( rStyleSettings.GetHighContrastMode() )
        {
            nOutputSlot = SID_OUTPUT_QUALITY_CONTRAST;
        }
        else
        {
            nOutputSlot = SID_OUTPUT_QUALITY_COLOR;
        }

        if( rStyleSettings.GetHighContrastMode() && aAccOptions.GetIsForPagePreviews() )
        {
            nPreviewSlot = SID_PREVIEW_QUALITY_CONTRAST;
        }
        else
        {
            nPreviewSlot = SID_PREVIEW_QUALITY_COLOR;
        }

        mpViewSh->GetViewFrame()->GetDispatcher()->Execute( nOutputSlot, SfxCallMode::ASYNCHRON );
        mpViewSh->GetViewFrame()->GetDispatcher()->Execute( nPreviewSlot, SfxCallMode::ASYNCHRON );
    }

    mpViewSh->Invalidate();
}

IMPL_LINK( View, OnParagraphInsertedHdl, ::Outliner::ParagraphHdlParam, aParam, void )
{
    SdrObject* pObj = GetTextEditObject();

    if( aParam.pPara && pObj )
    {
        SdPage* pPage = dynamic_cast< SdPage* >( pObj->getSdrPageFromSdrObject() );
        if( pPage )
            pPage->onParagraphInserted( aParam.pOutliner, aParam.pPara, pObj );
    }
}

/**
 * Handler for the deletion of the pages (paragraphs).
 */
IMPL_LINK( View, OnParagraphRemovingHdl, ::Outliner::ParagraphHdlParam, aParam, void )
{
    SdrObject* pObj = GetTextEditObject();

    if( aParam.pPara && pObj )
    {
        SdPage* pPage = dynamic_cast< SdPage* >( pObj->getSdrPageFromSdrObject() );
        if( pPage )
            pPage->onParagraphRemoving( aParam.pOutliner, aParam.pPara, pObj );
    }
}

bool View::isRecordingUndo() const
{
    if( mrDoc.IsUndoEnabled() )
    {
        sd::UndoManager* pUndoManager = mrDoc.GetUndoManager();
        return pUndoManager && pUndoManager->IsInListAction();
    }
    else
    {
        return false;
    }
}

void View::AddCustomHdl()
{
    maSmartTags.addCustomHandles( maHdlList );
}

void View::updateHandles()
{
    AdjustMarkHdl();
}

SdrViewContext View::GetContext() const
{
    SdrViewContext eContext = SdrViewContext::Standard;
    if( maSmartTags.getContext( eContext ) )
        return eContext;
    else
        return FmFormView::GetContext();
}

bool View::HasMarkablePoints() const
{
    if( maSmartTags.HasMarkablePoints() )
        return true;
    else
        return FmFormView::HasMarkablePoints();
}

sal_Int32 View::GetMarkablePointCount() const
{
    sal_Int32 nCount = FmFormView::GetMarkablePointCount();
    nCount += maSmartTags.GetMarkablePointCount();
    return nCount;
}

bool View::HasMarkedPoints() const
{
    if( maSmartTags.HasMarkedPoints() )
        return true;
    else
        return FmFormView::HasMarkedPoints();
}

bool View::MarkPoint(SdrHdl& rHdl, bool bUnmark )
{
    if( maSmartTags.MarkPoint( rHdl, bUnmark ) )
        return true;
    else
        return FmFormView::MarkPoint( rHdl, bUnmark );
}

bool View::MarkPoints(const ::tools::Rectangle* pRect, bool bUnmark)
{
    if( maSmartTags.MarkPoints( pRect, bUnmark ) )
        return true;
    else
        return FmFormView::MarkPoints( pRect, bUnmark );
}

void View::CheckPossibilities()
{
    FmFormView::CheckPossibilities();
    maSmartTags.CheckPossibilities();
}

void View::OnBeginPasteOrDrop( PasteOrDropInfos* pInfo )
{
    SdrOutliner* pOutliner = GetTextEditOutliner();
    if (!pOutliner)
        return;

    // Turn character attributes of the paragraph of the insert position into
    // character-level attributes, so they are not lost when OnEndPasteOrDrop()
    // sets the paragraph stylesheet.
    SfxItemSet aSet(pOutliner->GetParaAttribs(pInfo->nStartPara));
    pOutliner->SetCharAttribs(pInfo->nStartPara, aSet);
}

/** this is called after a paste or drop operation, make sure that the newly inserted paragraphs
    get the correct style sheet. */
void View::OnEndPasteOrDrop( PasteOrDropInfos* pInfo )
{
    /* Style Sheet handling */
    SdrTextObj* pTextObj = GetTextEditObject();
    SdrOutliner* pOutliner = GetTextEditOutliner();
    if( !pOutliner || !pTextObj || !pTextObj->getSdrPageFromSdrObject() )
        return;

    SdPage* pPage = static_cast< SdPage* >( pTextObj->getSdrPageFromSdrObject() );
    const PresObjKind eKind = pPage->GetPresObjKind(pTextObj);

    // outline kinds are taken care of in Outliner::ImplSetLevelDependentStyleSheet
    if( eKind == PresObjKind::Outline )
        return;

    SfxStyleSheet* pStyleSheet = nullptr;
    if( eKind != PresObjKind::NONE )
        pStyleSheet = pPage->GetStyleSheetForPresObj(eKind);
    else
         pStyleSheet = pTextObj->GetStyleSheet();
    // just put the object style on each new paragraph
    for ( sal_Int32 nPara = pInfo->nStartPara; nPara <= pInfo->nEndPara; nPara++ )
    {
        pOutliner->SetStyleSheet( nPara, pStyleSheet );
    }
}

bool View::ShouldToggleOn(
    const bool bBulletOnOffMode,
    const bool bNormalBullet)
{
    // If setting bullets/numbering by the dialog, always should toggle on.
    if (!bBulletOnOffMode)
        return true;
    SdrModel* pSdrModel = GetModel();
    if (!pSdrModel)
        return false;

    bool bToggleOn = false;
    std::unique_ptr<SdrOutliner> pOutliner(SdrMakeOutliner(OutlinerMode::TextObject, *pSdrModel));
    const size_t nMarkCount = GetMarkedObjectCount();
    for (size_t nIndex = 0; nIndex < nMarkCount && !bToggleOn; ++nIndex)
    {
        SdrTextObj* pTextObj = dynamic_cast< SdrTextObj* >(GetMarkedObjectByIndex(nIndex));
        if (!pTextObj || pTextObj->IsTextEditActive())
            continue;
        if( dynamic_cast< const SdrTableObj *>( pTextObj ) !=  nullptr)
        {
            SdrTableObj* pTableObj = dynamic_cast< SdrTableObj* >(pTextObj);
            if (!pTableObj)
                continue;
            CellPos aStart, aEnd;
            SvxTableController* pTableController = dynamic_cast< SvxTableController* >(getSelectionController().get());
            if (pTableController)
            {
                pTableController->getSelectedCells(aStart, aEnd);
            }
            else
            {
                aStart = SdrTableObj::getFirstCell();
                aEnd = pTableObj->getLastCell();
            }
            sal_Int32 nColCount = pTableObj->getColumnCount();
            for (sal_Int32 nRow = aStart.mnRow; nRow <= aEnd.mnRow && !bToggleOn; nRow++)
            {
                for (sal_Int32 nCol = aStart.mnCol; nCol <= aEnd.mnCol && !bToggleOn; nCol++)
                {
                    sal_Int32 nCellIndex = nRow * nColCount + nCol;
                    SdrText* pText = pTableObj->getText(nCellIndex);
                    if (!pText || !pText->GetOutlinerParaObject())
                        continue;
                    pOutliner->SetText(*(pText->GetOutlinerParaObject()));
                    sal_Int16 nStatus = pOutliner->GetBulletsNumberingStatus();
                    bToggleOn = (bNormalBullet && nStatus != 0) || (!bNormalBullet && nStatus != 1);
                    pOutliner->Clear();
                }
            }
        }
        else
        {
            OutlinerParaObject* pParaObj = pTextObj->GetOutlinerParaObject();
            if (!pParaObj)
                continue;
            pOutliner->SetText(*pParaObj);
            sal_Int16 nStatus = pOutliner->GetBulletsNumberingStatus();
            bToggleOn = (bNormalBullet && nStatus != 0) || (!bNormalBullet && nStatus != 1);
            pOutliner->Clear();
        }
    }
    return bToggleOn;
}

void View::ChangeMarkedObjectsBulletsNumbering(
    const bool bToggle,
    const bool bHandleBullets,
    const SvxNumRule* pNumRule )
{
    SdrModel* pSdrModel = GetModel();
    OutputDevice* pOut = GetFirstOutputDevice();
    vcl::Window* pWindow = pOut ? pOut->GetOwnerWindow() : nullptr;
    if (!pSdrModel || !pWindow)
        return;

    const bool bUndoEnabled = pSdrModel->IsUndoEnabled();
    std::unique_ptr<SdrUndoGroup> pUndoGroup(bUndoEnabled ? new SdrUndoGroup(*pSdrModel) : nullptr);

    const bool bToggleOn = ShouldToggleOn( bToggle, bHandleBullets );

    std::unique_ptr<SdrOutliner> pOutliner(SdrMakeOutliner(OutlinerMode::TextObject, *pSdrModel));
    OutlinerView aOutlinerView(pOutliner.get(), pWindow);

    const size_t nMarkCount = GetMarkedObjectCount();
    for (size_t nIndex = 0; nIndex < nMarkCount; ++nIndex)
    {
        SdrTextObj* pTextObj = dynamic_cast< SdrTextObj* >(GetMarkedObjectByIndex(nIndex));
        if (!pTextObj || pTextObj->IsTextEditActive())
            continue;
        if( dynamic_cast< SdrTableObj *>( pTextObj ) !=  nullptr)
        {
            SdrTableObj* pTableObj = dynamic_cast< SdrTableObj* >(pTextObj);
            if (!pTableObj)
                continue;
            CellPos aStart, aEnd;
            SvxTableController* pTableController = dynamic_cast< SvxTableController* >(getSelectionController().get());
            if (pTableController)
            {
                pTableController->getSelectedCells(aStart, aEnd);
            }
            else
            {
                aStart = SdrTableObj::getFirstCell();
                aEnd = pTableObj->getLastCell();
            }
            sal_Int32 nColCount = pTableObj->getColumnCount();
            for (sal_Int32 nRow = aStart.mnRow; nRow <= aEnd.mnRow; nRow++)
            {
                for (sal_Int32 nCol = aStart.mnCol; nCol <= aEnd.mnCol; nCol++)
                {
                    sal_Int32 nCellIndex = nRow * nColCount + nCol;
                    SdrText* pText = pTableObj->getText(nCellIndex);
                    if (!pText || !pText->GetOutlinerParaObject())
                        continue;

                    pOutliner->SetText(*(pText->GetOutlinerParaObject()));
                    if (bUndoEnabled)
                    {
                        pUndoGroup->AddAction(pSdrModel->GetSdrUndoFactory().CreateUndoObjectSetText(*pTextObj, nCellIndex));
                    }
                    if ( !bToggleOn )
                    {
                        aOutlinerView.SwitchOffBulletsNumbering();
                    }
                    else
                    {
                        aOutlinerView.ApplyBulletsNumbering( bHandleBullets, pNumRule, bToggle );
                    }
                    sal_uInt32 nParaCount = pOutliner->GetParagraphCount();
                    pText->SetOutlinerParaObject(pOutliner->CreateParaObject(0, static_cast<sal_uInt16>(nParaCount)));
                    pOutliner->Clear();
                }
            }
            // Broadcast the object change event.
            if (!pTextObj->AdjustTextFrameWidthAndHeight())
            {
                pTextObj->SetChanged();
                pTextObj->BroadcastObjectChange();
            }
        }
        else
        {
            OutlinerParaObject* pParaObj = pTextObj->GetOutlinerParaObject();
            if (!pParaObj)
                continue;
            pOutliner->SetText(*pParaObj);
            if (bUndoEnabled)
            {
                pUndoGroup->AddAction(
                    pSdrModel->GetSdrUndoFactory().CreateUndoObjectSetText(*pTextObj, 0));
            }
            if ( !bToggleOn )
            {
                aOutlinerView.SwitchOffBulletsNumbering();
            }
            else
            {
                aOutlinerView.ApplyBulletsNumbering( bHandleBullets, pNumRule, bToggle );
            }
            sal_uInt32 nParaCount = pOutliner->GetParagraphCount();
            pTextObj->SetOutlinerParaObject(pOutliner->CreateParaObject(0, static_cast<sal_uInt16>(nParaCount)));
            pOutliner->Clear();
        }
    }

    if ( bUndoEnabled && pUndoGroup->GetActionCount() > 0 )
    {
        pSdrModel->BegUndo();
        pSdrModel->AddUndo(std::move(pUndoGroup));
        pSdrModel->EndUndo();
    }
}

} // end of namespace sd

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */

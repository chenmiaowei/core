/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "Transmitter.hxx"
#include "IBluetoothSocket.hxx"
#include <sal/log.hxx>

using namespace std;
using namespace osl; // Sockets etc.
using namespace sd;

Transmitter::Transmitter( IBluetoothSocket* aSocket )
  : pStreamSocket( aSocket ),
    mProcessingRequired(),
    mMutex(),
    mFinishRequested( false ),
    mLowPriority(),
    mHighPriority()
{
}

void SAL_CALL Transmitter::run()
{
    osl_setThreadName("bluetooth Transmitter");

    while ( true )
    {
        mProcessingRequired.wait();

        ::osl::MutexGuard aGuard( mMutex );

        if ( mFinishRequested ) {
            return;
        }
        if ( !mHighPriority.empty() )
        {
            OString aMessage( mHighPriority.front() );
            mHighPriority.pop();
            SAL_INFO( "sdremote.bluetooth", "write high prio line '" << aMessage << "'" );
            pStreamSocket->write( aMessage.getStr(), aMessage.getLength() );
        }
        else if ( !mLowPriority.empty() )
        {
            OString aMessage( mLowPriority.front() );
            mLowPriority.pop();
            SAL_INFO( "sdremote.bluetooth", "write normal line '" << aMessage << "'" );
            pStreamSocket->write( aMessage.getStr(), aMessage.getLength() );
        }

        if ( mLowPriority.empty() && mHighPriority.empty())
        {
            mProcessingRequired.reset();
        }
    }
}

void Transmitter::notifyFinished()
{
    ::osl::MutexGuard aGuard( mMutex );
    mFinishRequested = true;
    mProcessingRequired.set();
}

Transmitter::~Transmitter()
{
}

void Transmitter::addMessage( const OString& aMessage, const Priority aPriority )
{
    ::osl::MutexGuard aGuard( mMutex );
    switch ( aPriority )
    {
        case PRIORITY_LOW:
            mLowPriority.push( aMessage );
            break;
        case PRIORITY_HIGH:
            mHighPriority.push( aMessage );
            break;
    }
    if ( !mProcessingRequired.check() )
    {
        mProcessingRequired.set();
    }
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */

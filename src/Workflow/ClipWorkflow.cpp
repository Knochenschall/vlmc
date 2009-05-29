/*****************************************************************************
 * ClipWorkflow.cpp : Clip workflow. Will extract a single frame from a VLCMedia
 *****************************************************************************
 * Copyright (C) 2008-2009 the VLMC team
 *
 * Authors: Hugo Beauzee-Luyssen <hugo@vlmc.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include <QtDebug>

#include "ClipWorkflow.h"

ClipWorkflow::ClipWorkflow( Clip::Clip* clip, QMutex* renderMutex,
                            QMutex* condMutex, QWaitCondition* waitCond ) :
                m_clip( clip ),
                m_buffer( NULL ),
                m_renderMutex( renderMutex ),
                m_condMutex( condMutex ),
                m_waitCond( waitCond ),
                m_mediaPlayer(NULL),
                m_state( ClipWorkflow::Stopped )
{
    m_buffer = new unsigned char[VIDEOHEIGHT * VIDEOWIDTH * 4];
    m_stateLock = new QReadWriteLock();
}

ClipWorkflow::~ClipWorkflow()
{
    delete[] m_buffer;
    delete m_stateLock;
}

void    ClipWorkflow::scheduleStop()
{
    QWriteLocker        lock( m_stateLock );
    m_state = StopScheduled;
}

unsigned char*    ClipWorkflow::getOutput()
{
    QMutexLocker    lock( m_renderMutex );
    return m_buffer;
}

void    ClipWorkflow::lock( ClipWorkflow* clipWorkflow, void** pp_ret )
{
    //In any case, we give vlc a buffer to render in...
    //If we don't, segmentation fault will catch us and eat our brains !! ahem...
//    qDebug() << "Locking in ClipWorkflow::lock";
    clipWorkflow->m_renderMutex->lock();
//    qDebug() << clipWorkflow->getState();
    *pp_ret = clipWorkflow->m_buffer;
}

void    ClipWorkflow::unlock( ClipWorkflow* clipWorkflow )
{
    clipWorkflow->m_renderMutex->unlock();

    clipWorkflow->m_stateLock->lockForRead();
    if ( clipWorkflow->m_state == Rendering )
    {
        QMutexLocker    lock5( clipWorkflow->m_condMutex );
        clipWorkflow->m_stateLock->unlock();
        clipWorkflow->m_waitCond->wait( clipWorkflow->m_condMutex );
    }
    clipWorkflow->m_stateLock->unlock();
//    qDebug() << "UnLocking in ClipWorkflow::unlock";
}

void    ClipWorkflow::setVmem()
{
    char        buffer[32];

    //TODO: it would be good if we somehow backup the old media parameters to restore it later.
    m_clip->getParent()->getVLCMedia()->addOption( ":vout=vmem" );
    m_clip->getParent()->getVLCMedia()->setDataCtx( this );
    m_clip->getParent()->getVLCMedia()->setLockCallback( reinterpret_cast<LibVLCpp::Media::lockCallback>( &ClipWorkflow::lock ) );
    m_clip->getParent()->getVLCMedia()->setUnlockCallback( reinterpret_cast<LibVLCpp::Media::unlockCallback>( &ClipWorkflow::unlock ) );
    m_clip->getParent()->getVLCMedia()->addOption( ":vmem-chroma=RV24" );

    sprintf( buffer, ":vmem-width=%i", VIDEOWIDTH );
    m_clip->getParent()->getVLCMedia()->addOption( buffer );

    sprintf( buffer, ":vmem-height=%i", VIDEOHEIGHT );
    m_clip->getParent()->getVLCMedia()->addOption( buffer );

    sprintf( buffer, "vmem-pitch=%i", VIDEOWIDTH * 3 );
    m_clip->getParent()->getVLCMedia()->addOption( buffer );
}

void    ClipWorkflow::initialize( LibVLCpp::MediaPlayer* mediaPlayer )
{
    setState( Initializing );
    setVmem();
    m_mediaPlayer = mediaPlayer;
    m_mediaPlayer->setMedia( m_clip->getParent()->getVLCMedia() );

    connect( m_mediaPlayer, SIGNAL( playing() ), this, SLOT( setPositionAfterPlayback() ), Qt::DirectConnection );
    connect( m_mediaPlayer, SIGNAL( endReached() ), this, SLOT( endReached() ), Qt::DirectConnection );
    m_mediaPlayer->play();
}

void    ClipWorkflow::setPositionAfterPlayback()
{
    disconnect( m_mediaPlayer, SIGNAL( playing() ), this, SLOT( setPositionAfterPlayback() ) );
    connect( m_mediaPlayer, SIGNAL( positionChanged() ), this, SLOT( pauseAfterPlaybackStarted() ), Qt::DirectConnection );
    m_mediaPlayer->setPosition( m_clip->getBegin() );
}

void    ClipWorkflow::pauseAfterPlaybackStarted()
{
    disconnect( m_mediaPlayer, SIGNAL( positionChanged() ), this, SLOT( pauseAfterPlaybackStarted() ) );
    disconnect( m_mediaPlayer, SIGNAL( playing() ), this, SLOT( pauseAfterPlaybackStarted() ) );

    connect( m_mediaPlayer, SIGNAL( paused() ), this, SLOT( pausedMediaPlayer() ), Qt::DirectConnection );
    m_mediaPlayer->pause();

}

void    ClipWorkflow::pausedMediaPlayer()
{
    disconnect( m_mediaPlayer, SIGNAL( paused() ), this, SLOT( pausedMediaPlayer() ) );
    QWriteLocker        lock( m_stateLock );
    m_state = ClipWorkflow::Ready;
}

bool    ClipWorkflow::isReady() const
{
    QReadLocker lock( m_stateLock );
    return m_state == ClipWorkflow::Ready;
}

bool    ClipWorkflow::isEndReached() const
{
    QReadLocker lock( m_stateLock );
    return m_state == ClipWorkflow::EndReached;
}

bool    ClipWorkflow::isStopped() const
{
    QReadLocker lock( m_stateLock );
    return m_state == ClipWorkflow::Stopped;
}

ClipWorkflow::State     ClipWorkflow::getState() const
{
    QReadLocker lock( m_stateLock );
    return m_state;
}

void    ClipWorkflow::startRender()
{
    while ( isReady() == false )
        usleep( 50 );
    m_mediaPlayer->play();
    setState( Rendering );
}

void    ClipWorkflow::endReached()
{
    qDebug() << "End reached";
    setState( EndReached );
}

const Clip*     ClipWorkflow::getClip() const
{
    return m_clip;
}

void            ClipWorkflow::stop()
{
    QWriteLocker        lock( m_stateLock );
    qDebug() << "ClipWorkflow::stop()";
    Q_ASSERT( m_mediaPlayer != NULL );
    m_mediaPlayer->stop();
    m_mediaPlayer = NULL;
    //Don't use setState here since m_stateLock is already locked;
    m_state = Stopped;
}

void            ClipWorkflow::setPosition( float pos )
{
    m_mediaPlayer->setPosition( pos );
}

bool            ClipWorkflow::isRendering() const
{
    QReadLocker lock( m_stateLock );
    return m_state == ClipWorkflow::Rendering;
}

void            ClipWorkflow::setState( State state )
{
    QWriteLocker    lock( m_stateLock );
    m_state = state;
}

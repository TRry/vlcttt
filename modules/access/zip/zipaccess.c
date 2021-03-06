/*****************************************************************************
 * zipaccess.c: Module (access) to extract different archives, based on zlib
 *****************************************************************************
 * Copyright (C) 2009 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Jean-Philippe André <jpeg@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/** @todo:
 * - implement crypto (using url zip://user:password@path-to-archive!/file)
 * - read files in zip with long name (use unz_file_info.size_filename)
 * - multi-volume archive support ?
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <limits.h>
#include "zip.h"
#include <vlc_access.h>

/** **************************************************************************
 * This is our own access_sys_t for zip files
 *****************************************************************************/
struct access_sys_t
{
    /* zlib / unzip members */
    unzFile            zipFile;
};

static int AccessControl( access_t *p_access, int i_query, va_list args );
static ssize_t AccessRead( access_t *, void *, size_t );
static int AccessSeek( access_t *, uint64_t );
static char *unescapeXml( const char *psz_text );

/** **************************************************************************
 * \brief Unescape valid XML string
 * The exact reverse of escapeToXml (zipstream.c)
 *****************************************************************************/
static char *unescapeXml( const char *psz_text )
{
    char *psz_ret = malloc( strlen( psz_text ) + 1 );
    if( unlikely( !psz_ret ) ) return NULL;

    char *psz_tmp = psz_ret;
    for( char *psz_iter = (char*) psz_text; *psz_iter; ++psz_iter, ++psz_tmp )
    {
        if( *psz_iter == '?' )
        {
            int i_value;
            if( !sscanf( ++psz_iter, "%02x", &i_value ) )
            {
                /* Invalid number: URL incorrectly encoded */
                free( psz_ret );
                return NULL;
            }
            *psz_tmp = (char) i_value;
            psz_iter++;
        }
        else if( isAllowedChar( *psz_iter ) )
        {
            *psz_tmp = *psz_iter;
        }
        else
        {
            /* Invalid character encoding for the URL */
            free( psz_ret );
            return NULL;
        }
    }
    *psz_tmp = '\0';

    return psz_ret;
}

/** **************************************************************************
 * \brief Open access
 *****************************************************************************/
int AccessOpen( vlc_object_t *p_this )
{
    access_t     *p_access = (access_t*)p_this;
    access_sys_t *p_sys;
    int i_ret              = VLC_EGENERIC;

    char *psz_pathToZip = NULL, *psz_path = NULL, *psz_sep = NULL;
    char *psz_fileInzip = NULL;

    if( !strstr( p_access->psz_location, ZIP_SEP ) )
    {
        msg_Dbg( p_access, "location does not contain separator " ZIP_SEP );
        return VLC_EGENERIC;
    }

    p_access->p_sys = p_sys = (access_sys_t*)
            calloc( 1, sizeof( access_sys_t ) );
    if( unlikely( !p_sys ) )
        return VLC_ENOMEM;

    /* Split the MRL */
    psz_path = xstrdup( p_access->psz_location );
    psz_sep = strstr( psz_path, ZIP_SEP );

    *psz_sep = '\0';
    psz_pathToZip = unescapeXml( psz_path );
    if( !psz_pathToZip )
    {
        /* Maybe this was not an encoded string */
        msg_Dbg( p_access, "not an encoded URL  Trying file '%s'",
                 psz_path );
        psz_pathToZip = strdup( psz_path );
        if( unlikely( !psz_pathToZip ) )
        {
            i_ret = VLC_ENOMEM;
            goto exit;
        }
    }

    psz_fileInzip = unescapeXml( psz_sep + ZIP_SEP_LEN );
    if( unlikely( psz_fileInzip == NULL ) )
    {
        psz_fileInzip = strdup( psz_sep + ZIP_SEP_LEN );
        if( unlikely( psz_fileInzip == NULL ) )
        {
            i_ret = VLC_ENOMEM;
            goto exit;
        }
    }

    /* Define IO functions */
    zlib_filefunc_def func;
    func.zopen_file   = ZipIO_Open;
    func.zread_file   = ZipIO_Read;
    func.zwrite_file  = ZipIO_Write; // see comment
    func.ztell_file   = ZipIO_Tell;
    func.zseek_file   = ZipIO_Seek;
    func.zclose_file  = ZipIO_Close;
    func.zerror_file  = ZipIO_Error;
    func.opaque       = p_access;

    /* Open zip archive */
    p_sys->zipFile = unzOpen2( psz_pathToZip, &func );
    if( !p_sys->zipFile )
    {
        msg_Err( p_access, "not a valid zip archive: '%s'", psz_pathToZip );
        goto exit;
    }

    /* Open file in zip */
    if( unzLocateFile( p_sys->zipFile, psz_fileInzip, 0 ) != UNZ_OK )
    {
        msg_Err( p_access, "could not [re]locate file in zip: '%s'",
                 psz_fileInzip );
        goto exit;
    }

    if( unzOpenCurrentFile( p_sys->zipFile ) != UNZ_OK )
    {
        msg_Err( p_access, "could not [re]open file in zip: '%s'",
                 psz_fileInzip );
        goto exit;
    }

    /* Set callback */
    ACCESS_SET_CALLBACKS( AccessRead, NULL, AccessControl, AccessSeek );

    i_ret = VLC_SUCCESS;

exit:
    if( i_ret != VLC_SUCCESS )
    {
        if( p_sys->zipFile )
        {
            unzCloseCurrentFile( p_sys->zipFile );
            unzClose( p_sys->zipFile );
        }
        free( p_sys );
    }

    free( psz_fileInzip );
    free( psz_pathToZip );
    free( psz_path );
    return i_ret;
}

/** **************************************************************************
 * \brief Close access: free structures
 *****************************************************************************/
void AccessClose( vlc_object_t *p_this )
{
    access_t     *p_access = (access_t*)p_this;
    access_sys_t *p_sys = p_access->p_sys;
    unzFile file = p_sys->zipFile;

    unzCloseCurrentFile( file );
    unzClose( file );
    free( p_sys );
}

/** **************************************************************************
 * \brief Control access
 *****************************************************************************/
static int AccessControl( access_t *p_access, int i_query, va_list args )
{
    access_sys_t *sys = p_access->p_sys;
    bool         *pb_bool;
    int64_t      *pi_64;

    switch( i_query )
    {
        case STREAM_CAN_SEEK:
        case STREAM_CAN_PAUSE:
        case STREAM_CAN_CONTROL_PACE:
            pb_bool = (bool*)va_arg( args, bool* );
            *pb_bool = true;
            break;

        case STREAM_CAN_FASTSEEK:
            pb_bool = (bool*)va_arg( args, bool* );
            *pb_bool = false;
            break;

        case STREAM_GET_SIZE:
        {
            unz_file_info z_info;

            unzGetCurrentFileInfo( sys->zipFile, &z_info,
                                   NULL, 0, NULL, 0, NULL, 0 );
            *va_arg( args, uint64_t * ) = z_info.uncompressed_size;
            break;
        }

        case STREAM_GET_PTS_DELAY:
            pi_64 = (int64_t*)va_arg( args, int64_t * );
            *pi_64 = DEFAULT_PTS_DELAY;
            break;

        case STREAM_SET_PAUSE_STATE:
            /* Nothing to do */
            break;

        default:
            return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

/** **************************************************************************
 * \brief Read access
 * Reads current opened file in zip. This does not open the file in zip.
 * Return -1 if no data yet, 0 if no more data, else real data read
 *****************************************************************************/
static ssize_t AccessRead( access_t *p_access, void *p_buffer, size_t sz )
{
    access_sys_t *p_sys = p_access->p_sys;
    unzFile file = p_sys->zipFile;

    int i_read = unzReadCurrentFile( file, p_buffer, sz );

    return ( i_read >= 0 ? i_read : VLC_EGENERIC );
}

/** **************************************************************************
 * \brief Seek inside zip file
 *****************************************************************************/
static int AccessSeek( access_t *p_access, uint64_t seek_len )
{
    access_sys_t *p_sys = p_access->p_sys;
    unzFile file = p_sys->zipFile;

    if( seek_len > ULONG_MAX )
        return VLC_EGENERIC; /* TODO: update minizip to LFS */

    if( unzSetOffset( file, seek_len ) < 0 )
        return VLC_EGENERIC;

    return VLC_SUCCESS;
}

/** **************************************************************************
 * \brief I/O functions for the ioapi: open (read only)
 *****************************************************************************/
static void* ZCALLBACK ZipIO_Open( void* opaque, const char* file, int mode )
{
    assert(opaque != NULL);
    assert(mode == (ZLIB_FILEFUNC_MODE_READ | ZLIB_FILEFUNC_MODE_EXISTING));

    access_t *p_access = (access_t*) opaque;

    char *fileUri = malloc( strlen(file) + 8 );
    if( unlikely( !fileUri ) )
        return NULL;
    if( !strstr( file, "://" ) )
    {
        strcpy( fileUri, "file://" );
        strcat( fileUri, file );
    }
    else
    {
        strcpy( fileUri, file );
    }

    stream_t *s = vlc_stream_NewURL( p_access, fileUri );
    free( fileUri );
    return s;
}

/** **************************************************************************
 * \brief I/O functions for the ioapi: read
 *****************************************************************************/
static uLong ZCALLBACK ZipIO_Read( void* opaque, void* stream,
                                   void* buf, uLong size )
{
    (void)opaque;
    //access_t *p_access = (access_t*) opaque;
    //msg_Dbg(p_access, "read %d", size);
    return vlc_stream_Read( (stream_t*) stream, buf, size );
}

/** **************************************************************************
 * \brief I/O functions for the ioapi: write (assert insteadof segfault)
 *****************************************************************************/
static uLong ZCALLBACK ZipIO_Write( void* opaque, void* stream,
                                    const void* buf, uLong size )
{
    (void)opaque; (void)stream; (void)buf; (void)size;
    int zip_access_cannot_write_this_should_not_happen = 0;
    assert(zip_access_cannot_write_this_should_not_happen);
    return 0;
}

/** **************************************************************************
 * \brief I/O functions for the ioapi: tell
 *****************************************************************************/
static long ZCALLBACK ZipIO_Tell( void* opaque, void* stream )
{
    (void)opaque;
    int64_t i64_tell = vlc_stream_Tell( (stream_t*) stream );
    //access_t *p_access = (access_t*) opaque;
    //msg_Dbg(p_access, "tell %" PRIu64, i64_tell);
    return (long)i64_tell;
}

/** **************************************************************************
 * \brief I/O functions for the ioapi: seek
 *****************************************************************************/
static long ZCALLBACK ZipIO_Seek( void* opaque, void* stream,
                                  uLong offset, int origin )
{
    (void)opaque;
    int64_t pos = offset;
    switch( origin )
    {
        case SEEK_CUR:
            pos += vlc_stream_Tell( (stream_t*) stream );
            break;
        case SEEK_SET:
            break;
        case SEEK_END:
            pos += stream_Size( (stream_t*) stream );
            break;
        default:
            return -1;
    }
    if( pos < 0 )
        return -1;
    vlc_stream_Seek( (stream_t*) stream, pos );
    /* Note: in unzip.c, unzlocal_SearchCentralDir seeks to the end of
             the stream, which is doable but returns an error in VLC.
             That's why we always assume this was OK. FIXME */
    return 0;
}

/** **************************************************************************
 * \brief I/O functions for the ioapi: close
 *****************************************************************************/
static int ZCALLBACK ZipIO_Close( void* opaque, void* stream )
{
    (void)opaque;
    vlc_stream_Delete( (stream_t*) stream );
    return 0;
}

/** **************************************************************************
 * \brief I/O functions for the ioapi: test error (man 3 ferror)
 *****************************************************************************/
static int ZCALLBACK ZipIO_Error( void* opaque, void* stream )
{
    (void)opaque;
    (void)stream;
    //msg_Dbg( p_access, "error" );
    return 0;
}

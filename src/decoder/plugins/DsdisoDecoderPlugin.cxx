#include "config.h"
#include "DsdisoDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "util/BitReverse.hxx"
#include "util/ByteOrder.hxx"
#include "util/StringView.hxx"
#include "fs/Path.hxx"
#include "fs/AllocatedPath.hxx"
#include "fs/FileSystem.hxx"
#include "tag/Handler.hxx"
#include "tag/Builder.hxx"
#include "song/DetachedSong.hxx"
#include "DsdLib.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include "libsacd/sacd_reader.h"
#include "libsacd/sacd_disc.h"
#include "libsacd/version.h"
#include "libdstdec/dst_decoder_mt.h"

const Domain dsdiso_domain("dsdiso");

AllocatedPath dsd_file_last{nullptr};

class DSDISO_HANDLE
{
 private:
    iso_media_t * m_pDsdIsoMedia;
    dst_decoder_t *m_pDstDecoder;
     vector < uint8_t > m_arrDstBuf;
     vector < uint8_t > m_arrDsdBuf;
    int m_nDsdBufSize;
    int m_nDstBufSize;
    int m_nDsdSamplerate;
    int m_nFramerate;
    string strDsdIsoPath;
 public:

    //dsf_output_handle_t dsf_ft;

    int m_nTracks;
    int m_nPcmOutChannels;
    sacd_disc_t *m_pSacdReader;
    
    uint8_t *pDsdDataOut;
    size_t nDsdSizeOut;

     DSDISO_HANDLE()
     {
        m_pDsdIsoMedia = nullptr;
        m_pSacdReader = nullptr;
        m_pDstDecoder = nullptr;
        m_nTracks = 0;
    }
    ~DSDISO_HANDLE()
    {
        if(m_pSacdReader)
        {
            m_pDsdIsoMedia->iso_close();
            delete m_pSacdReader;
        }
        if(m_pDsdIsoMedia)
        {
            delete m_pDsdIsoMedia;
        }
        if(m_pDstDecoder)
        {
            delete m_pDstDecoder;
        }
    }

    int open(string p_path)
    {
        m_pDsdIsoMedia = new iso_media_t();

        if(!m_pDsdIsoMedia)
        {
            fprintf(stderr, "PANIC: exception_overflow\n");
            return 0;
        }

        m_pSacdReader = new sacd_disc_t;
        if(!m_pSacdReader)
        {
            fprintf(stderr, "PANIC: exception_overflow\n");
            return 0;
        }

        if(!m_pDsdIsoMedia->iso_open(p_path.c_str()))
        {
            fprintf(stderr, "PANIC: exception_io_data\n");
            return 0;
        }

        if((m_nTracks = m_pSacdReader->open(m_pDsdIsoMedia)) == 0)
        {
            m_pDsdIsoMedia->iso_close();
            fprintf(stderr, "PANIC: Failed to parse SACD media\n");
            return 0;
        }
        strDsdIsoPath = p_path;
        return m_nTracks;
    }

    string init(uint32_t nSubsong, area_id_e nArea)
    {
        if(m_pDstDecoder)
        {
            delete m_pDstDecoder;
            m_pDstDecoder = nullptr;
        }
        string strFileName = m_pSacdReader->set_track(nSubsong, nArea, 0);
        m_nDsdSamplerate = m_pSacdReader->get_samplerate();
        m_nFramerate = m_pSacdReader->get_framerate();
        m_nPcmOutChannels = m_pSacdReader->get_channels();
        m_nDstBufSize = m_nDsdBufSize = m_nDsdSamplerate / 8 / m_nFramerate * m_nPcmOutChannels;
        m_arrDsdBuf.resize(m_nDsdBufSize);
        m_arrDstBuf.resize(m_nDstBufSize);
        
        pDsdDataOut = nullptr;
        nDsdSizeOut = 0;
        return strFileName;
    }

    int decode(void)
    {
        uint8_t *pDsdData;
        uint8_t *pDstData;
        size_t nDsdSize = 0;
        size_t nDstSize = 0;
        int decode_end = 0;
        
        pDsdDataOut = nullptr;
        nDsdSizeOut = 0;
        //while(1)
        {
            pDsdData = m_arrDsdBuf.data();
            pDstData = m_arrDstBuf.data();
            nDstSize = m_nDstBufSize;
            frame_type_e nFrameType;

            if(m_pSacdReader->read_frame(pDstData, &nDstSize, &nFrameType))
            {
                if(nDstSize > 0)
                {
                    if(nFrameType == FRAME_INVALID)
                    {
                        nDstSize = m_nDstBufSize;
                        memset(pDstData, 0x69, nDstSize);
                    }
                    if(nFrameType == FRAME_DST)
                    {
                        if(!m_pDstDecoder)
                        {
                            printf("create dst decoder\n");
                            m_pDstDecoder = new dst_decoder_t();
                            if(!m_pDstDecoder)
                            {
                                return -1;
                            }
                            if(m_pDstDecoder->init(m_nPcmOutChannels, m_nDsdSamplerate, m_nFramerate) != 0)
                            {
                                delete m_pDstDecoder;
                                m_pDstDecoder = nullptr;
                                return -1;
                                //return true;
                            }
                        }
                        m_pDstDecoder->decode(pDstData, nDstSize, &pDsdData, &nDsdSize);
                    }
                    else
                    {
                        pDsdData = pDstData;
                        nDsdSize = nDstSize;
                    }
                    if(nDsdSize > 0)
                    {
                        //dsf_write_frame(&dsf_ft, pDsdData, nDsdSize);
                        pDsdDataOut = pDsdData;
                        nDsdSizeOut = nDsdSize;
                        return 1;
                    }
                }
            }
        }
        pDsdData = nullptr;
        pDstData = nullptr;
        nDstSize = 0;
        if(m_pDstDecoder)
        {
            m_pDstDecoder->decode(pDstData, nDstSize, &pDsdData, &nDsdSize);
        }
        if(nDsdSize > 0)
        {
            //dsf_write_frame(&dsf_ft, pDsdData, nDsdSize);
            pDsdDataOut = pDsdData;
            nDsdSizeOut = nDsdSize;
        }
        else
        {
            pDsdDataOut = nullptr;
            nDsdSizeOut = 0;
        }
        if(!m_pDstDecoder)
        {
            delete m_pDstDecoder;
            m_pDstDecoder = nullptr;
        }
        return 2;
    }
};

static bool dsdiso_init(const ConfigBlock & block)
{
    FormatDebug(dsdiso_domain, "############ dsdiso_init");
    return true;
}

static void dsdiso_finish() noexcept
{
    FormatDebug(dsdiso_domain, "############ dsdiso_finish");
}

static std::forward_list < DetachedSong > dsdiso_container(Path path_fs)
{
    std::forward_list < DetachedSong > list;
    TagBuilder tag_builder;
    auto tail = list.before_begin();
    auto suffix = path_fs.GetSuffix();
    Path pt = path_fs.GetDirectoryName();
    FormatDebug(dsdiso_domain, "############ dsdiso_container: [%s]%s", suffix, path_fs.c_str());
    FormatDebug(dsdiso_domain, "############ dsdiso_container: [DIR]%s", pt.c_str());

    DSDISO_HANDLE *pDsdIsoHandle = new DSDISO_HANDLE();
    if(!pDsdIsoHandle)
    {
        FormatDebug(dsdiso_domain, "new DSDISO_HANDLE() failed");
        return list;
    }
    if(!pDsdIsoHandle->open(path_fs.c_str()))
    {
        FormatDebug(dsdiso_domain, "open Dsdiso failed: %s\n", path_fs.c_str());
        return list;
    }
    auto nTrackCount = pDsdIsoHandle->m_pSacdReader->get_track_count(AREA_TWOCH);
    if(nTrackCount > 0)
    {
        for(auto track = 0u; track < nTrackCount; track++)
        {
            unsigned s;
            char track_name[64];
            auto tag_value = std::to_string(track + 1);

            AddTagHandler handler(tag_builder);
            TrackDetails cTrackDetails;

            scarletbook_area_t *cArea = pDsdIsoHandle->m_pSacdReader->get_area(AREA_TWOCH);
            pDsdIsoHandle->m_pSacdReader->getTrackDetails(track, AREA_TWOCH, &cTrackDetails);

            handler.OnTag(TAG_TRACK, tag_value.c_str());
            handler.OnTag(TAG_ARTIST, cTrackDetails.strArtist.data());
            handler.OnTag(TAG_ALBUM, cTrackDetails.strAlbum.data());
            handler.OnTag(TAG_ALBUM_ARTIST, cTrackDetails.strAlbumArtist.data());

            handler.OnTag(TAG_TITLE, cTrackDetails.strTitle.data());
            s = cArea->area_tracklist_time->duration[track].minutes * 60 + cArea->area_tracklist_time->duration[track].seconds;
            handler.OnDuration(SongTime::FromS(s));

            std::sprintf(track_name, "TRACK%02d.iso", track + 1);
            tail = list.emplace_after(tail, track_name, tag_builder.Commit());
        }
    }
    delete pDsdIsoHandle;
    return list;
}


static void dsdiso_decode(DecoderClient & client, Path path_fs)
{
    //get suffix "iso"
    auto suffix = path_fs.GetSuffix();
    //get file name
    const auto pt = path_fs.GetDirectoryName();
    //get track index
    string ptr = path_fs.c_str();
    string strTrackIdx = ptr.substr(ptr.length() - 6, 2);
    auto track = atoi(strTrackIdx.c_str()) - 1;
    //init
    FormatDebug(dsdiso_domain, "############ dsdiso_decode: [%s]%s", suffix, path_fs.c_str());
    FormatDebug(dsdiso_domain, "############ dsdiso_decode: [DIR]%s", pt.c_str());
    FormatDebug(dsdiso_domain, "############ dsdiso_decode: track = %d", track);
    DSDISO_HANDLE *pDsdIsoHandle = new DSDISO_HANDLE();
    if(!pDsdIsoHandle)
    {
        FormatError(dsdiso_domain, "new DSDISO_HANDLE() failed");
        return;
    }
    FormatDebug(dsdiso_domain, "############ dsdiso_decode: new");
    if(!pDsdIsoHandle->open(pt.c_str()))
    {
        FormatError(dsdiso_domain, "open Dsdiso failed: %s\n", pt.c_str());
        return;
    }
    FormatDebug(dsdiso_domain, "############ dsdiso_decode: open");
    
    auto dsd_fs     = pDsdIsoHandle->m_pSacdReader->get_samplerate();
    auto dsd_ch     = 2;//pDsdIsoHandle->m_pSacdReader->get_channels();
    auto dsd_kpbs   = dsd_ch * dsd_fs / 1000;
    
    scarletbook_area_t *cArea = pDsdIsoHandle->m_pSacdReader->get_area(AREA_TWOCH);
    
    unsigned s = cArea->area_tracklist_time->duration[track].minutes * 60 + cArea->area_tracklist_time->duration[track].seconds;
    
    float s_all = float(s) + 0.5f;
    float s_cnt = 0.0f;
    float bytePreSec = (float)(dsd_ch * dsd_fs / 8);
    
    unsigned long m_lsnPreSec = cArea->area_tracklist_offset->track_length_lsn[track] / s;
    FormatError(dsdiso_domain, "############ dsdiso_decode: check: track=%d; t=%d; fs=%d; ch=%d; kbps=%d; lsn/s=%ld", track, s, dsd_fs, dsd_ch, dsd_kpbs, m_lsnPreSec);
    
    AudioFormat audio_format = CheckAudioFormat(dsd_fs / 8, SampleFormat::DSD, dsd_ch);
    SongTime songtime = SongTime::FromS(s);
        
    FormatDebug(dsdiso_domain, "############ dsdiso_decode: ready");
    client.Ready(audio_format, true, songtime);
    
    FormatDebug(dsdiso_domain, "############ dsdiso_decode: start");
    
    pDsdIsoHandle->init(track, AREA_TWOCH);

    while(1)
    {
        int ret = pDsdIsoHandle->decode();
        if(ret > 0)
        {
            if(ret == 2)
            {
                break;
            }
            //if(param_lsbitfirst)
            //{
            //    bit_reverse_buffer(dsx_buf.data(), dsx_buf.data() + dsx_buf.size());
            //}
            auto cmd = client.SubmitData(nullptr, pDsdIsoHandle->pDsdDataOut, pDsdIsoHandle->nDsdSizeOut, dsd_kpbs);
            if(cmd == DecoderCommand::SEEK)
            {
                auto seconds = client.GetSeekTime().ToDoubleS();
                FormatDebug(dsdiso_domain, "############ dsdiso_decode: %.3f", seconds);
                pDsdIsoHandle->m_pSacdReader->seek_play((signed long)seconds * m_lsnPreSec);
                //pDsdIsoHandle->m_pSacdReader->seek_play2(seconds);
                //if(sacd_reader->seek(seconds))
                //{
                    client.CommandFinished();
                //}
                //else
                //{
                //    client.SeekError();
                //}
                cmd = client.GetCommand();
                s_cnt = (float)seconds;
            }
            else
            {
                s_cnt += (float)pDsdIsoHandle->nDsdSizeOut / bytePreSec;
            }
            if(s_cnt > s_all)
            {
                break;
            }
            if(cmd == DecoderCommand::STOP)
            {
                break;
            }
        }
        else
        {
            break;
        }
    }
    delete pDsdIsoHandle;
}

static bool dsdiso_scan(Path path_fs, TagHandler & handler)
{
    auto suffix = path_fs.GetSuffix();
    Path pt = path_fs.GetDirectoryName();

    FormatDebug(dsdiso_domain, "############ dsdiso_scan: [%s]%s", suffix, path_fs.c_str());
    FormatDebug(dsdiso_domain, "############ dsdiso_scan: [DIR]%s", pt.c_str());
    DSDISO_HANDLE *pDsdIsoHandle = new DSDISO_HANDLE();
    if(!pDsdIsoHandle)
    {
        FormatDebug(dsdiso_domain, "new DSDISO_HANDLE() failed");
        return false;
    }
    if(!pDsdIsoHandle->open(path_fs.c_str()))
    {
        delete pDsdIsoHandle;
        FormatDebug(dsdiso_domain, "open Dsdiso failed: %s\n", path_fs.c_str());
        return false;
    }
    auto nTrackCount = pDsdIsoHandle->m_pSacdReader->get_track_count(AREA_TWOCH);
    if(nTrackCount > 0)
    {
        unsigned s = 0;
        unsigned s_total = 0;
        for(auto track = 0u; track < nTrackCount; track++)
        {
            scarletbook_area_t *cArea = pDsdIsoHandle->m_pSacdReader->get_area(AREA_TWOCH);
            s = cArea->area_tracklist_time->duration[track].minutes * 60 + cArea->area_tracklist_time->duration[track].seconds;
            s_total += s;
        }
        handler.OnDuration(SongTime::FromS(s_total));
    }
    else
    {
        delete pDsdIsoHandle;
        return false;
    }
    delete pDsdIsoHandle;
    return true;
}

static const char *const dsdiso_suffixes[] = {
    //"dat",
    "iso",
    nullptr
};

static const char *const dsdiso_mime_types[] = {
    //"application/x-dat",
    "application/x-iso",
    //"application/x-dff",
    //"audio/x-dsd",
    //"audio/x-dff",
    nullptr
};

constexpr DecoderPlugin dsdiso_decoder_plugin =
    DecoderPlugin("dsdiso", dsdiso_decode, dsdiso_scan)
    .WithInit(dsdiso_init, dsdiso_finish)
    .WithContainer(dsdiso_container)
    .WithSuffixes(dsdiso_suffixes)
    .WithMimeTypes(dsdiso_mime_types);

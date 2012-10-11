#ifndef SNARK_SENSORS_DC1394_H_
#define SNARK_SENSORS_DC1394_H_

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/optional.hpp>
#include <dc1394/dc1394.h>

#include <opencv2/core/core.hpp>
#include <opencv2/core/core_c.h>
#include <comma/io/select.h>
#include <comma/visiting/traits.h>
#include <snark/sensors/dc1394/types.h>

namespace snark { namespace camera {

/// image acquisition from dc1394 camera
class dc1394
{
public:
    struct config
    {
        enum output_type{ RGB, BGR, Raw };
        config();
        int type() const;
        
        output_type output;
        dc1394video_mode_t  video_mode;
        dc1394operation_mode_t operation_mode;
        dc1394speed_t iso_speed;
        // TODO framerate is not used in format7, as the way to set the framerate is different.
        // see http://damien.douxchamps.net/ieee1394/libdc1394/v2.x/faq/#How_do_I_set_the_frame_rate
        dc1394framerate_t frame_rate;
        uint64_t guid;
    };

    dc1394( const config& config = config(), unsigned int format7_width = 0, unsigned int format7_height = 0, unsigned int format7_size = 8160, unsigned int exposure = 0 );
    ~dc1394();

    const cv::Mat& read();
    boost::posix_time::ptime time() const { return m_time; }
    bool poll();
    static void list_cameras();

private:
    void init_camera();
    void setup_camera();
    void setup_camera_format7();
    
    config m_config;
    dc1394camera_t* m_camera;
    dc1394video_frame_t* m_frame;
    dc1394video_frame_t m_output_frame;
    cv::Mat m_image;
    const boost::posix_time::ptime m_epoch;
    boost::posix_time::ptime m_time;
    int m_fd;
    comma::io::select m_select;
    boost::posix_time::time_duration m_frame_duration;
    unsigned int m_width;
    unsigned int m_height;
    unsigned int m_format7_width;
    unsigned int m_format7_height;
    unsigned int m_format7_size;
    boost::optional< unsigned int > m_auto_exposure;
    boost::optional< unsigned int > m_adjusted_exposure;
    boost::posix_time::ptime m_last_shutter_update;
    
};

} } // namespace snark { namespace camera {

namespace comma { namespace visiting {

template <> struct traits< snark::camera::dc1394::config >
{
    template < typename Key, class Visitor >
    static void visit( const Key&, snark::camera::dc1394::config& c, Visitor& v )
    {
        std::string outputType;
        v.apply( "output-type", outputType );
        if( outputType == "RGB" || outputType == "rgb" )
        {
            c.output = snark::camera::dc1394::config::RGB;
        }
        else if( outputType == "BGR" || outputType == "brg" )
        {
            c.output = snark::camera::dc1394::config::BGR;
        }
        else 
        {
            c.output = snark::camera::dc1394::config::Raw;
        }

        std::string video_mode;
        std::string operation_mode;
        std::string iso_speed;
        std::string frame_rate;
        v.apply( "video-mode", video_mode );
        v.apply( "operation-mode", operation_mode );
        v.apply( "iso-speed", iso_speed );
        v.apply( "frame-rate", frame_rate );

        c.video_mode = snark::camera::video_mode_from_string( video_mode );
        c.operation_mode = snark::camera::operation_mode_from_string( operation_mode );
        c.iso_speed = snark::camera::iso_speed_from_string( iso_speed );
        c.frame_rate = snark::camera::frame_rate_from_string( frame_rate );
        
        v.apply( "guid", c.guid );
    }

    template < typename Key, class Visitor >
    static void visit( const Key&, const snark::camera::dc1394::config& c, Visitor& v )
    {
        std::string outputType;
        if( c.output == snark::camera::dc1394::config::RGB )
        {
            outputType = "RGB";
        }
        else if( c.output == snark::camera::dc1394::config::BGR )
        {
            outputType = "BGR";
        }
        else
        {
            outputType = "Raw";
        }
        v.apply( "output-type", outputType );

        std::string video_mode = snark::camera::video_mode_to_string( c.video_mode );
        std::string operation_mode = snark::camera::operation_mode_to_string( c.operation_mode );
        std::string iso_speed = snark::camera::iso_speed_to_string( c.iso_speed );
        std::string frame_rate = snark::camera::frame_rate_to_string( c.frame_rate );

        v.apply( "video-mode", video_mode );
        v.apply( "operation-mode", operation_mode );
        v.apply( "iso-speed", iso_speed );
        v.apply( "frame-rate", frame_rate );
        v.apply( "guid", c.guid );
    }
};

} }

#endif // SNARK_SENSORS_DC1394_H_
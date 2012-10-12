#include <pcap.h>
#include <boost/array.hpp>
#include <boost/optional.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_real.hpp>
#include <boost/random/variate_generator.hpp>
#include <boost/shared_ptr.hpp>
#include <comma/application/command_line_options.h>
#include <comma/application/signal_flag.h>
#include <comma/base/exception.h>
#include <comma/base/types.h>
#include <comma/io/publisher.h>
#include <comma/math/compare.h>
#include <comma/name_value/parser.h>
#include <comma/string/string.h>
#include <snark/sensors/velodyne/stream.h>
#include <snark/sensors/velodyne/thin/thin.h>
#include <snark/sensors/velodyne/impl/pcap_reader.h>
#include <snark/sensors/velodyne/impl/proprietary_reader.h>
#include <snark/sensors/velodyne/impl/stream_traits.h>
#include <snark/sensors/velodyne/impl/udp_reader.h>
#include <snark/sensors/velodyne/thin/scan.h>

using namespace snark;

static void usage()
{
    std::cerr << std::endl;
    std::cerr << "Takes velodyne packets on stdin and outputs thinned packets to stdout" << std::endl;
    std::cerr << std::endl;
    std::cerr << "Usage: cat velodyne*.bin | velodyne-thin <options>" << std::endl;
    std::cerr << "       netcat shrimp.littleboard 12345 | velodyne-thin <options>" << std::endl;
    std::cerr << std::endl;
    std::cerr << "velodyne options" << std::endl;
    std::cerr << "    --db <velodyne db.xml file>: default /usr/local/etc/db.xml" << std::endl;
    std::cerr << std::endl;
    std::cerr << "data flow options" << std::endl;
    std::cerr << "    --output-raw: if present, output uncompressed thinned packets" << std::endl;
    std::cerr << "    --pcap: if present, velodyne data is read from pcap packets" << std::endl;
    std::cerr << "             e.g: cat velo.pcap | velodyne-thin <options> --pcap" << std::endl;
    std::cerr << "    --publish=<address>: if present, publish on given address (see io-publish -h for address syntax)" << std::endl;
    std::cerr << "    --verbose,-v" << std::endl;
    std::cerr << std::endl;
    std::cerr << "filtering options" << std::endl;
    std::cerr << "    --udp-port <port>: if present, read raw velodyne packets from udp and timestamp them" << std::endl;
    std::cerr << "    --rate <rate>: thinning rate between 0 and 1" << std::endl;
    std::cerr << "                    default 1: send all valid datapoints" << std::endl;
    std::cerr << "    --scan-rate <rate>: scan thin rate between 0 and 1" << std::endl;
    std::cerr << "    --focus <options>: focus on particular region" << std::endl;
    std::cerr << "                        e.g. at choosen --rate in the direction of" << std::endl;
    std::cerr << "                        0 degrees 30 degrees wide not farther than 10 metres" << std::endl;
    std::cerr << "                        output 80% points in the focus region and 20% the rest" << std::endl;
    std::cerr << "                        --focus=\"sector;range=10;bearing=0;ken=30;ratio=0.8\"" << std::endl;
    std::cerr << "                        todo: currently only \"sector\" type implemented" << std::endl;
    std::cerr << "    --subtract-by-age <options>: subtract background at given rate, e.g." << std::endl;
    std::cerr << "                        show background only:" << std::endl;
    std::cerr << "                        --subtract-by-age=\"foreground=0.0;background=1.0\"" << std::endl;
    std::cerr << "                        show 20% of foreground points and 5% of background points:" << std::endl;
    std::cerr << "                        --subtract-by-age=\"foreground=0.2;background=0.05\"" << std::endl;
    std::cerr << "                        important: works for stationary velodyne only!" << std::endl;
    std::cerr << "    --subtract-max-range <options>: subtract background at given rate, e.g." << std::endl;
    std::cerr << "                        show foreground only, tolerance of distance to background 2cm:" << std::endl;
    std::cerr << "                        --subtract-max-range=\"foreground=1.0;background=0.0;epsilon=0.02\"" << std::endl;
    std::cerr << "    --subtract <filename>: load background from file" << std::endl;
    std::cerr << std::endl;
    exit( -1 );
}

static bool verbose = false;
static bool outputRaw = false;
static boost::optional< float > rate;
static boost::optional< double > scanRate;
static boost::optional< double > angularSpeed_;
static boost::optional< velodyne::db > db;
static boost::scoped_ptr< velodyne::thin::Focus > focus;
static boost::scoped_ptr< velodyne::thin::Background > background;
static boost::scoped_ptr< velodyne::thin::MaxRangeBackground > maxRangeBackground;
static boost::scoped_ptr< velodyne::thin::FixedBackground > fixedBackground;
static velodyne::thin::scan scan;
static boost::scoped_ptr< comma::io::publisher > publisher;

static double angularSpeed( const snark::velodyne::packet& packet )
{
    if( angularSpeed_ ) { return *angularSpeed_; }
    double da = double( packet.blocks[0].rotation() - packet.blocks[11].rotation() ) / 100;
    double dt = double( ( snark::velodyne::impl::timeOffset( 0, 0 ) - snark::velodyne::impl::timeOffset( 11, 0 ) ).total_microseconds() ) / 1e6;
    return da / dt;
}

static velodyne::thin::Focus* makeFocus( const std::string& options, double rate ) // quick and dirty
{
    std::string type = comma::name_value::map( options, "type" ).value< std::string >( "type" );
    double ratio = comma::name_value::map( options ).value( "ratio", 1.0 );
    velodyne::thin::region* region;
    if( type == "sector" ) { region = new velodyne::thin::Sector( comma::name_value::parser().get< velodyne::thin::Sector >( options ) ); }
    else { COMMA_THROW( comma::exception, "expected type (sector), got " << type ); }
    velodyne::thin::Focus* focus = new velodyne::thin::Focus( rate, ratio );
    focus->insert( 0, region );
    return focus;
}

static velodyne::thin::Background* makeBackground( const std::string& options ) // quick and dirty
{
    float backgroundRate = comma::name_value::map( options ).value( "background", 0.0 );
    float foregroundRate = comma::name_value::map( options ).value( "foreground", 1.0 );
    boost::posix_time::time_duration age = boost::posix_time::milliseconds( static_cast< int >( comma::name_value::map( options ).value( "age", 10.0 ) * 1000 ) );
    boost::posix_time::time_duration threshold = boost::posix_time::milliseconds( static_cast< int >( comma::name_value::map( options ).value( "threshold", 1.0 ) * 1000 ) );
    if( comma::math::less( 1.0, backgroundRate + foregroundRate ) ) { COMMA_THROW( comma::exception, "expected fore- and background rates sum of which less than 1, got " << options ); }
    return new velodyne::thin::Background( age, threshold, foregroundRate, backgroundRate );
}

static velodyne::thin::MaxRangeBackground* makeMaxRangeBackground( const std::string& options ) // quick and dirty
{
    float backgroundRate = comma::name_value::map( options ).value( "background", 0.0 );
    float foregroundRate = comma::name_value::map( options ).value( "foreground", 1.0 );
    unsigned int epsilon = comma::name_value::map( options ).value( "epsilon", 0.0 ) * 500;
    if( comma::math::less( 1.0, backgroundRate + foregroundRate ) ) { COMMA_THROW( comma::exception, "expected fore- and background rates sum of which less than 1, got " << options ); }
    return new velodyne::thin::MaxRangeBackground( epsilon, foregroundRate, backgroundRate );
}


static velodyne::thin::FixedBackground* makeFixedBackground( const std::string& filename ) // quick and dirty
{
    velodyne::thin::FixedBackground* background = new velodyne::thin::FixedBackground( rate ? *rate : 1.0 );
    snark::proprietary_reader stream( filename );
    while( true )
    {
        const char* p = stream.read();
        if( p == NULL ) { break; }
        const velodyne::packet& packet = *( reinterpret_cast< const velodyne::packet* >( p ) );
        bool upper = true;
        for( unsigned int block = 0; block < packet.blocks.size(); ++block, upper = !upper )
        {
            double rotation = double( packet.blocks[block].rotation() ) / 100;
            for( unsigned int laser = 0; laser < packet.blocks[block].lasers.size(); ++laser )
            {
                unsigned int id = upper ? laser : laser + packet.blocks[block].lasers.size();
                comma::uint16 angle = velodyne::impl::azimuth( rotation, laser, angularSpeed( packet ) ) * 100;
                comma::uint16 range = packet.blocks[block].lasers[laser].range(); //double range = db.lasers[r.id].range( r.range );
                background->update( id, range, angle );
            }
        }
    }
    return background;
}

template < typename S >
void run( S* stream )
{
    static const unsigned int timeSize = 12;
    boost::mt19937 generator;
    boost::uniform_real< float > distribution( 0, 1 );
    boost::variate_generator< boost::mt19937&, boost::uniform_real< float > > random( generator, distribution );
    comma::uint64 count = 0;
    double compression = 0;
    velodyne::packet packet;
    comma::signal_flag isShutdown;
    while( !isShutdown && std::cin.good() && !std::cin.eof() && std::cout.good() && !std::cout.eof() )
    {
        const char* p = velodyne::impl::streamTraits< S >::read( *stream, sizeof( velodyne::packet ) );
        if( p == NULL ) { break; }
        ::memcpy( &packet, p, velodyne::packet::size );
        boost::posix_time::ptime timestamp = stream->timestamp();
        if( scanRate ) { scan.thin( packet, *scanRate, angularSpeed( packet ) ); }
        if( !scan.empty() )
        {
            if( focus ) { velodyne::thin::thin( packet, *focus, *db, angularSpeed( packet ), random ); }
            else if( background ) { velodyne::thin::thin( packet, timestamp, *background, *db, angularSpeed( packet ), random ); }
            else if( fixedBackground ) { velodyne::thin::thin( packet, *fixedBackground, *db, angularSpeed( packet ), random ); }
            else if( maxRangeBackground ) { velodyne::thin::thin( packet, *maxRangeBackground, *db, angularSpeed( packet ), random ); }
            else if( rate ) { velodyne::thin::thin( packet, *rate, random ); }
        }
        const boost::posix_time::ptime base( snark::timing::epoch );
        const boost::posix_time::time_duration d = timestamp - base;
        comma::int64 seconds = d.total_seconds();
        comma::int32 nanoseconds = static_cast< comma::int32 >( d.total_microseconds() % 1000000 ) * 1000;
        if( outputRaw ) // real quick and dirty
        {
            static boost::array< char, 16 + timeSize + velodyne::packet::size + 4 > buf;
            static const boost::array< char, 2 > start = {{ -78, 85 }}; // see QLib::Bytestreams::GetDefaultStartDelimiter()
            static const boost::array< char, 2 > end = {{ 117, -97 }}; // see QLib::Bytestreams::GetDefaultStartDelimiter()
            ::memcpy( &buf[0], &start[0], 2 );
            ::memcpy( &buf[0] + buf.size() - 2, &end[0], 2 );
            ::memcpy( &buf[0] + 16, &seconds, 8 );
            ::memcpy( &buf[0] + 16 + 8, &nanoseconds, 4 );
            ::memcpy( &buf[0] + 16 + 8 + 4, &packet, velodyne::packet::size );
            if( publisher )
            {
                publisher->write( &buf[0], buf.size() );
            }
            else
            {
                std::cout.write( &buf[0], buf.size() );
            }
        }
        else
        {
            static char buf[ timeSize + sizeof( comma::uint16 ) + velodyne::thin::maxBufferSize ];
            comma::uint16 size = timeSize + velodyne::thin::serialize( packet, buf + timeSize + sizeof( comma::uint16 ) );
            ::memcpy( buf, &size, sizeof( comma::uint16 ) );
            ::memcpy( buf + sizeof( comma::uint16 ), &seconds, sizeof( comma::int64 ) );
            ::memcpy( buf + sizeof( comma::uint16 ) + sizeof( comma::int64 ), &nanoseconds, sizeof( comma::int32 ) );
            if( publisher )
            {
                publisher->write( buf, size + sizeof( comma::uint16 ) );
            }
            else
            {
                std::cout.write( buf, size + sizeof( comma::uint16 ) );
            }
            if( verbose )
            {
                ++count;
                compression = 0.9 * compression + 0.1 * ( double( size + sizeof( comma::int16 ) ) / ( velodyne::packet::size + timeSize ) );
                if( count % 10000 == 0 ) { std::cerr << "velodyne-thin: processed " << count << " packets; compression rate " << compression << std::endl; }
            }
        }
    }
    if( publisher ) { publisher->close(); }
    std::cerr << "velodyne-thin: " << ( isShutdown ? "signal received" : "no more data" ) << "; shutdown" << std::endl;
}

int main( int ac, char** av )
{
    try
    {
        comma::command_line_options options( ac, av );
        if( options.exists( "--help,-h" ) ) { usage(); }
        outputRaw = options.exists( "--output-raw" );
        rate = options.optional< float >( "--rate" );
        scanRate = options.optional< double >( "--scan-rate" );
        if( options.exists( "--publish" ) ) { publisher.reset( new comma::io::publisher( options.value< std::string >( "--publish" ), comma::io::mode::binary ) ); }
        options.assert_mutually_exclusive( "--focus,--subtract-by-age,--subtract-max-range,--subtract" );
        if( options.exists( "--focus,--subtract-by-age,--subtract-max-range,--subtract" ) )
        {
            db = velodyne::db( options.value< std::string >( "--db", "/usr/local/etc/db.xml" ) );
        }
        if( options.exists( "--focus" ) )
        {
            focus.reset( makeFocus( options.value< std::string >( "--focus" ), rate ? *rate : 1.0 ) );
            std::cerr << "velodyne-thin: rate in focus: " << focus->rateInFocus() << "; rate out of focus: " << focus->rateOutOfFocus() << "; coverage: " << focus->coverage() << std::endl;
        }
        else if( options.exists( "--subtract-by-age" ) )
        {
            background.reset( makeBackground( options.value< std::string >( "--subtract-by-age" ) ) );
        }
        else if( options.exists( "--subtract-max-range" ) )
        {
            maxRangeBackground.reset( makeMaxRangeBackground( options.value< std::string >( "--subtract-max-range" ) ) );
        }
        else if( options.exists( "--subtract" ) )
        {
            fixedBackground.reset( makeFixedBackground( options.value< std::string >( "--subtract" ) ) );
        }
        verbose = options.exists( "--verbose,-v" );
        #ifdef WIN32
        _setmode( _fileno( stdin ), _O_BINARY );
        _setmode( _fileno( stdout ), _O_BINARY );
        #endif
        options.assert_mutually_exclusive( "--pcap,--udp-port" );
        boost::optional< unsigned short > port = options.optional< unsigned short >( "--udp-port" );
        if( port ) { run( new snark::udp_reader( *port ) ); }
        else if( options.exists( "--pcap" ) ) { run( new snark::pcap_reader ); }
        else { run( new snark::proprietary_reader ); }
        return 0;
    }
    catch( std::exception& ex ) { std::cerr << "velodyne-thin: " << ex.what() << std::endl; }
    catch( ... ) { std::cerr << "velodyne-thin: unknown exception" << std::endl; }
    usage();
}
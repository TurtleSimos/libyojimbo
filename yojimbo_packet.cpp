/*
    Yojimbo Client/Server Network Library.
    
    Copyright © 2016, The Network Protocol Company, Inc.

    Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

        1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

        2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer 
           in the documentation and/or other materials provided with the distribution.

        3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived 
           from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
    WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
    USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "yojimbo_packet.h"
#include "yojimbo_allocator.h"

#ifdef _MSC_VER
#include <malloc.h>
#else
#include <alloca.h>
#endif

namespace yojimbo
{
    int WritePacket( const PacketReadWriteInfo & info, Packet * packet, uint8_t * buffer, int bufferSize )
    {
        assert( packet );
        assert( buffer );
        assert( bufferSize > 0 );
        assert( info.protocolId );
        assert( info.packetFactory );

        const int numPacketTypes = info.packetFactory->GetNumPacketTypes();

        WriteStream stream( buffer, bufferSize );

        stream.SetContext( info.context );

        for ( int i = 0; i < info.prefixBytes; ++i )
        {
            uint8_t zero = 0;
            stream.SerializeBits( zero, 8 );
        }

        uint32_t crc32 = 0;

        if ( !info.rawFormat )
            stream.SerializeBits( crc32, 32 );

        int packetType = packet->GetType();

        assert( numPacketTypes > 0 );

        if ( numPacketTypes > 1 )
        {
            stream.SerializeInteger( packetType, 0, numPacketTypes - 1 );
        }

        if ( !packet->SerializeInternal( stream ) )
        {
            debug_printf( "serialize packet type %d failed (write packet)\n", packetType );
            return 0;
        }

        stream.SerializeCheck( "end of packet" );

        stream.Flush();

        if ( !info.rawFormat )
        {
            uint32_t network_protocolId = host_to_network( info.protocolId );
            crc32 = calculate_crc32( (uint8_t*) &network_protocolId, 4 );
            crc32 = calculate_crc32( buffer + info.prefixBytes, stream.GetBytesProcessed() - info.prefixBytes, crc32 );
            *((uint32_t*)(buffer+info.prefixBytes)) = host_to_network( crc32 );
        }

        if ( stream.GetError() )
        {
            debug_printf( "stream error %d (write packet)\n", stream.GetError() );
            return 0;
        }

        return stream.GetBytesProcessed();
    }

    Packet * ReadPacket( const PacketReadWriteInfo & info, const uint8_t * buffer, int bufferSize, int * errorCode )
    {
        assert( buffer );
        assert( bufferSize > 0 );
        assert( info.protocolId != 0 );
        assert( info.packetFactory );

        if ( errorCode )
            *errorCode = YOJIMBO_PROTOCOL_ERROR_NONE;

        ReadStream stream( buffer, bufferSize );

        stream.SetContext( info.context );

        for ( int i = 0; i < info.prefixBytes; ++i )
        {
            uint32_t dummy = 0;
            stream.SerializeBits( dummy, 8 );
        }

        uint32_t read_crc32 = 0;
        if ( !info.rawFormat )
        {
            stream.SerializeBits( read_crc32, 32 );

            uint32_t network_protocolId = host_to_network( info.protocolId );
            uint32_t crc32 = calculate_crc32( (const uint8_t*) &network_protocolId, 4 );
            uint32_t zero = 0;
            crc32 = calculate_crc32( (const uint8_t*) &zero, 4, crc32 );
            crc32 = calculate_crc32( buffer + info.prefixBytes + 4, bufferSize - 4 - info.prefixBytes, crc32 );

            if ( crc32 != read_crc32 )
            {
                debug_printf( "corrupt packet. expected crc32 %x, got %x (read packet)\n", crc32, read_crc32 );
                if ( errorCode )
                    *errorCode = YOJIMBO_PROTOCOL_ERROR_CRC32_MISMATCH;
                return NULL;
            }
        }

        int packetType = 0;

        const int numPacketTypes = info.packetFactory->GetNumPacketTypes();

        assert( numPacketTypes > 0 );

        if ( numPacketTypes > 1 )
        {
            if ( !stream.SerializeInteger( packetType, 0, numPacketTypes - 1 ) )
            {
                debug_printf( "invalid packet type %d (read packet)\n", packetType );
                if ( errorCode )
                    *errorCode = YOJIMBO_PROTOCOL_ERROR_INVALID_PACKET_TYPE;
                return NULL;
            }
        }

        if ( info.allowedPacketTypes )
        {
            if ( !info.allowedPacketTypes[packetType] )
            {
                debug_printf( "packet type %d not allowed (read packet)\n", packetType );
                if ( errorCode )
                    *errorCode = YOJIMBO_PROTOCOL_ERROR_PACKET_TYPE_NOT_ALLOWED;
                return NULL;
            }
        }

        Packet *packet = info.packetFactory->CreatePacket( packetType );
        if ( !packet )
        {
            debug_printf( "create packet type %d failed (read packet)\n", packetType );
            if ( errorCode )
                *errorCode = YOJIMBO_PROTOCOL_ERROR_CREATE_PACKET_FAILED;
            return NULL;
        }

        if ( !packet->SerializeInternal( stream ) )
        {
            debug_printf( "serialize packet type %d failed (read packet)\n", packetType );
            if ( errorCode )
                *errorCode = YOJIMBO_PROTOCOL_ERROR_SERIALIZE_PACKET_FAILED;
            goto cleanup;
        }

#if YOJIMBO_SERIALIZE_CHECKS
        if ( !stream.SerializeCheck( "end of packet" ) )
        {
            debug_printf( "serialize check failed at end of packet type %d (read packet)\n", packetType );
            if ( errorCode )
                *errorCode = YOJIMBO_PROTOCOL_ERROR_SERIALIZE_CHECK_FAILED;
            goto cleanup;
        }
#endif // #if YOJIMBO_SERIALIZE_CHECKS

        if ( stream.GetError() )
        {
            debug_printf( "stream error %d (read packet)\n", stream.GetError() );
            if ( errorCode )
                *errorCode = stream.GetError();
            goto cleanup;
        }

        return packet;

cleanup:
        info.packetFactory->DestroyPacket( packet );
        return NULL;
    }

    PacketFactory::PacketFactory( Allocator & allocator, int numPacketTypes )
    {
        m_numPacketTypes = numPacketTypes;
        m_numAllocatedPackets = 0;
        m_allocator = &allocator;
    }

    PacketFactory::~PacketFactory()
    {
#if YOJIMBO_DEBUG_PACKET_LEAKS
        if ( allocated_packets.size() )
        {
            printf( "you leaked packets!\n" );
            printf( "%d packets leaked\n", m_numAllocatedPackets );
            typedef std::map<void*,int>::iterator itor_type;
            for ( itor_type i = allocated_packets.begin(); i != allocated_packets.end(); ++i ) 
            {
                printf( "leaked packet %p (type %d)\n", i->first, i->second );
            }
            exit(1);
        }
#endif // #if YOJIMBO_DEBUG_PACKET_LEAKS

        assert( m_numAllocatedPackets == 0 );
    }

    Packet * PacketFactory::CreatePacket( int type )
    {
        assert( type >= 0 );
        assert( type < m_numPacketTypes );

        Packet * packet = CreateInternal( type );
        if ( !packet )
            return NULL;

#if YOJIMBO_DEBUG_PACKET_LEAKS
        allocated_packets[packet] = type;
        assert( allocated_packets.find( packet ) != allocated_packets.end() );
#endif // #if YOJIMBO_DEBUG_PACKET_LEAKS
        
        m_numAllocatedPackets++;

        return packet;
    }

    void PacketFactory::DestroyPacket( Packet * packet )
    {
        assert( packet );

        if ( !packet )
            return;

#if YOJIMBO_DEBUG_PACKET_LEAKS
        assert( allocated_packets.find( packet ) != allocated_packets.end() );
        allocated_packets.erase( packet );
#endif // #if YOJIMBO_DEBUG_PACKET_LEAKS

        assert( m_numAllocatedPackets > 0 );

        m_numAllocatedPackets--;

        YOJIMBO_DELETE( *m_allocator, Packet, packet );
    }

    int PacketFactory::GetNumPacketTypes() const
    {
        return m_numPacketTypes;
    }

    void PacketFactory::SetPacketType( Packet * packet, int type )
    {
        if ( packet )
            packet->SetType( type );
    }

    Allocator & PacketFactory::GetAllocator()
    {
        assert( m_allocator );
        return *m_allocator;
    }
}

/*
	restinio
*/

/*!
	Helper output context for writing buffers to output stream (socket).
*/

#pragma once

#include <vector>

#include <restinio/asio_include.hpp>

#include <restinio/buffers.hpp>
#include <restinio/optional.hpp>
#include <restinio/variant.hpp>
#include <restinio/impl/sendfile_operation.hpp>

namespace restinio
{

namespace impl
{

using asio_bufs_container_t = std::vector< asio_ns::const_buffer >;

//
// write_group_output_ctx_t
//

//! Helper class for writting response data.
class write_group_output_ctx_t
{
	//! Get the maximum number of buffers that can be written with
	//! gather write operation.
	static constexpr auto
	max_iov_len()
	{
		using len_t = decltype( asio_ns::detail::max_iov_len );
		return std::min< len_t >( asio_ns::detail::max_iov_len, 64 );
	}

	public:
		//! Contruct an object.
		/*
			Space for m_asio_bufs is reserved to be ready to store max_iov_len() asio bufs.
		*/
		write_group_output_ctx_t()
		{
			m_asio_bufs.reserve( max_iov_len() );
		}

		//! Trivial write operaton.
		/*!
			Presented with a vector of ordinary buffers (data-size objects).
		*/
		class trivial_write_operation_t
		{
				friend class write_group_output_ctx_t;

				explicit trivial_write_operation_t(
					const asio_bufs_container_t & asio_bufs )
					:	m_asio_bufs{ &asio_bufs }
				{}

			public:
				trivial_write_operation_t( const trivial_write_operation_t & ) = default;
				trivial_write_operation_t & operator = ( const trivial_write_operation_t & ) = default;

				trivial_write_operation_t( trivial_write_operation_t && ) = default;
				trivial_write_operation_t & operator = ( trivial_write_operation_t && ) = default;

				//! Get buffer "iovec" for performing gather write.
				const std::vector< asio_ns::const_buffer > &
				get_trivial_bufs() const noexcept
				{
					return *m_asio_bufs;
				}

			private:
				const asio_bufs_container_t * m_asio_bufs;
		};

		//! Write operaton using sendfile.
		class file_write_operation_t
		{
				friend class write_group_output_ctx_t;

				explicit file_write_operation_t(
					const sendfile_t & sendfile,
					sendfile_operation_shared_ptr_t & sendfile_operation )
					:	m_sendfile{ &sendfile }
					,	m_sendfile_operation{ &sendfile_operation }
				{}

			public:
				file_write_operation_t( const file_write_operation_t & ) = default;
				file_write_operation_t & operator = ( const file_write_operation_t & ) = default;

				file_write_operation_t( file_write_operation_t && ) = default;
				file_write_operation_t & operator = ( file_write_operation_t && ) = default;

				template< typename Socket, typename After_Write_CB >
				void
				start_sendfile_operation(
					asio_ns::executor executor,
					Socket & socket,
					After_Write_CB after_sendfile_cb ) const
				{
					assert( m_sendfile->is_valid() );

					if( !m_sendfile->is_valid() )
					{
						// This must never happen.
						throw exception_t{ "invalid file descriptor in sendfile operation." };
					}

					auto sendfile_operation =
						std::make_shared< sendfile_operation_runner_t< Socket > >(
							*m_sendfile,
							std::move( executor ),
							socket,
							std::move( after_sendfile_cb ) );

					*m_sendfile_operation = std::move( sendfile_operation );
					(*m_sendfile_operation)->start();
				}

				auto
				timelimit() const
				{
					assert( m_sendfile->is_valid() );

					return m_sendfile->timelimit();
				}

				void
				reset()
				{
					m_sendfile_operation->reset();
				}

				//! Get the size of sendfile operation.
				auto size() const noexcept { return m_sendfile->size(); }

			private:
				const sendfile_t * m_sendfile;
				// Storage must be external.
				sendfile_operation_shared_ptr_t * m_sendfile_operation;
		};

		//! None write operation.
		struct none_write_operation_t {};

		friend class solid_write_operation_t;

		//! Check if data is trunsmitting now
		bool transmitting() const { return static_cast< bool >( m_current_wg ); }

		//! Start handlong next write group.
		bool
		start_next_write_group( optional_t< write_group_t > next_wg )
		{
			m_current_wg = std::move( next_wg );
			return transmitting();
		}

		//! An alias for variant holding write operation specifics.
		using solid_write_operation_variant_t =
			variant_t<
				none_write_operation_t,
				trivial_write_operation_t,
				file_write_operation_t >;

		//! et an object with next write operation to perform.
		solid_write_operation_variant_t
		extract_next_write_operation()
		{
			assert( m_current_wg );

			solid_write_operation_variant_t result{ none_write_operation_t{} };

			if( m_next_writable_item_index < m_current_wg->items_count() )
			{
				// Has writable items.
				const auto next_wi_type =
					m_current_wg->items()[ m_next_writable_item_index ].write_type();

				if( writable_item_type_t::trivial_write_operation == next_wi_type )
				{
					// Trivial buffers.
					result = prepare_trivial_buffers_wo();
				}
				else
				{
					// Sendfile.
					assert( writable_item_type_t::file_write_operation == next_wi_type );
					result = prepare_sendfile_wo();
				}
			}

			return result;
		}

		void
		fail_write_group( const asio_ns::error_code & ec )
		{
			assert( m_current_wg );

			invoke_after_write_notificator_if_necessary( ec );
			m_current_wg.reset();
			m_sendfile_operation.reset();
		}

		//! Finish writing group normally.
		void
		finish_write_group()
		{
			assert( m_current_wg );

			invoke_after_write_notificator_if_necessary( asio_ns::error_code{} );
			m_current_wg.reset();
		}

	private:
		//! Reset the write group and associated context.
		void
		reset_write_group()
		{
			m_current_wg.reset();
			m_next_writable_item_index = 0;
		}

		//! Execute notification callback if necessary.
		void
		invoke_after_write_notificator_if_necessary( const asio_ns::error_code & ec )
		{
			try
			{
				if( m_current_wg->after_write_notificator() )
				{
					m_current_wg->after_write_notificator()( ec );
				}
			}
			catch( const std::exception & ex )
			{
				// Actualy no need to reset m_current_wg as a thrown exception
				// will break working circle of connection.
				// But as it is used as flag for transmitting()
				// we reset the object.
				reset_write_group();

				throw exception_t{
					fmt::format( "after write callback failed: {}", ex.what() ) };
			}
		}

		//! Prepare write operation for trivial buffers.
		trivial_write_operation_t
		prepare_trivial_buffers_wo()
		{
			m_asio_bufs.clear();

			const auto & items = m_current_wg->items();

			for( ;m_next_writable_item_index < items.size() &&
				writable_item_type_t::trivial_write_operation ==
					items[ m_next_writable_item_index ].write_type() &&
				max_iov_len() > m_asio_bufs.size();
				++m_next_writable_item_index )
			{
				m_asio_bufs.emplace_back( items[ m_next_writable_item_index ].buf() );
			}

			assert( !m_asio_bufs.empty() );
			return trivial_write_operation_t{ m_asio_bufs };
		}

		//! Prepare write operation for sendfile.
		file_write_operation_t
		prepare_sendfile_wo()
		{
			const auto & sf =
				m_current_wg->items()[ m_next_writable_item_index++ ].sendfile_operation();

			return file_write_operation_t{ sf, m_sendfile_operation };
		}

		//! Real buffers with data.
		optional_t< write_group_t > m_current_wg;

		//! Keeps track of the next writable item stored in m_current_wg.
		/*!
			When emitting next solid write operation
			we need to know where the next starting item is.
		*/
		std::size_t m_next_writable_item_index{ 0 };

		//! Asio buffers storage.
		asio_bufs_container_t m_asio_bufs;

		//! Sendfile operation storage context.
		sendfile_operation_shared_ptr_t m_sendfile_operation;
};

} /* namespace impl */

} /* namespace restinio */
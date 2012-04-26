////////////////////////////////////////////////////////////////////////////////
/// @brief datafiles
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2011 triagens GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
/// @author Copyright 2011, triagens GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "headers.h"

#include <VocBase/document-collection.h>

// -----------------------------------------------------------------------------
// --SECTION--                                                     private types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief number of headers per block
////////////////////////////////////////////////////////////////////////////////

#define NUMBER_HEADERS_PER_BLOCK (1000000)

// -----------------------------------------------------------------------------
// --SECTION--                                                     private types
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief headers
////////////////////////////////////////////////////////////////////////////////

typedef struct simple_headers_s {
  TRI_headers_t base;

  TRI_doc_mptr_t const* _freelist;
  TRI_vector_pointer_t _blocks;

  size_t _headerSize;
}
simple_headers_t;

// -----------------------------------------------------------------------------
// --SECTION--                                                 private functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief clears an header
////////////////////////////////////////////////////////////////////////////////

static void ClearSimpleHeaders (TRI_doc_mptr_t* header, size_t headerSize) {
  assert(header);
  memset(header, 0, headerSize);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief requests a header
////////////////////////////////////////////////////////////////////////////////

static TRI_doc_mptr_t* RequestSimpleHeaders (TRI_headers_t* h) {
  simple_headers_t* headers = (simple_headers_t*) h;
  char const* header;
  union { TRI_doc_mptr_t const* c; TRI_doc_mptr_t* h; } c;

  if (headers->_freelist == NULL) {
    char* begin;
    char* ptr;

    begin = TRI_Allocate(TRI_UNKNOWN_MEM_ZONE, NUMBER_HEADERS_PER_BLOCK * headers->_headerSize);
    if (!begin) {
      // out of memory
      TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);
      return NULL;
    }

    ptr = begin + headers->_headerSize * (NUMBER_HEADERS_PER_BLOCK - 1);

    header = NULL;

    for (;  begin <= ptr;  ptr -= headers->_headerSize) {
      ClearSimpleHeaders((TRI_doc_mptr_t*) ptr, headers->_headerSize);
      ((TRI_doc_mptr_t*) ptr)->_data = header;
      header = ptr;
    }

    headers->_freelist = (TRI_doc_mptr_t*) header;

    TRI_PushBackVectorPointer(&headers->_blocks, begin);
  }

  c.c = headers->_freelist;
  headers->_freelist = c.c->_data;

  c.h->_data = NULL;
  return c.h;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief verifies if header is still valid, possible returning a new one
////////////////////////////////////////////////////////////////////////////////

static TRI_doc_mptr_t* VerifySimpleHeaders (TRI_headers_t* h, TRI_doc_mptr_t* header) {
  return header;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief releases a header, putting it back onto the freelist
////////////////////////////////////////////////////////////////////////////////

static void ReleaseSimpleHeaders (TRI_headers_t* h, TRI_doc_mptr_t* header) {
  simple_headers_t* headers = (simple_headers_t*) h;

  ClearSimpleHeaders(header, headers->_headerSize);

  header->_data = headers->_freelist;
  headers->_freelist = header;
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                                  public functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @addtogroup VocBase
/// @{
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// @brief creates a new simple headers structures
////////////////////////////////////////////////////////////////////////////////

TRI_headers_t* TRI_CreateSimpleHeaders (size_t headerSize) {
  simple_headers_t* headers = TRI_Allocate(TRI_UNKNOWN_MEM_ZONE, sizeof(simple_headers_t));

  if (!headers) {
    TRI_set_errno(TRI_ERROR_OUT_OF_MEMORY);
    return NULL;
  }

  headers->base.request = RequestSimpleHeaders;
  headers->base.verify = VerifySimpleHeaders;
  headers->base.release = ReleaseSimpleHeaders;

  headers->_freelist = NULL;
  headers->_headerSize = headerSize;

  TRI_InitVectorPointer(&headers->_blocks, TRI_UNKNOWN_MEM_ZONE);

  return &headers->base;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a simple headers structures, but does not free the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_DestroySimpleHeaders (TRI_headers_t* h) {
  simple_headers_t* headers = (simple_headers_t*) h;
  size_t i;

  for (i = 0;  i < headers->_blocks._length;  ++i) {
    TRI_Free(TRI_UNKNOWN_MEM_ZONE, headers->_blocks._buffer[i]);
  }

  TRI_DestroyVectorPointer(&headers->_blocks);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroys a simple headers structures, but does not free the pointer
////////////////////////////////////////////////////////////////////////////////

void TRI_FreeSimpleHeaders (TRI_headers_t* headers) {
  TRI_DestroySimpleHeaders(headers);
  TRI_Free(TRI_UNKNOWN_MEM_ZONE, headers);
}

// -----------------------------------------------------------------------------
// --SECTION--                                               protected functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief iterates over all headers
////////////////////////////////////////////////////////////////////////////////

void TRI_IterateSimpleHeaders (TRI_headers_t* headers,
                               void (*iterator)(TRI_doc_mptr_t const*, void*),
                               void* data) {
  simple_headers_t* h = (simple_headers_t*) headers;
  size_t i;

  for (i = 0;  i < h->_blocks._length;   ++i) {
    TRI_doc_mptr_t* begin = h->_blocks._buffer[i];
    TRI_doc_mptr_t* end = begin + NUMBER_HEADERS_PER_BLOCK;

    for (;  begin < end;  ++begin) {
      iterator(begin, data);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @}
////////////////////////////////////////////////////////////////////////////////

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|// --SECTION--\\|/// @\\}\\)"
// End:

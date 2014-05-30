////////////////////////////////////////////////////////////////////////////////
/// @brief WAL read operation
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2004-2013 triAGENS GmbH, Cologne, Germany
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
/// @author Jan Steemann
/// @author Copyright 2011-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "Wal/ReadOperation.h"
#include "Wal/LogfileManager.h"

using namespace triagens::wal;

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief create a read operation
////////////////////////////////////////////////////////////////////////////////

ReadOperation::ReadOperation (void** address) 
  : _address(reinterpret_cast<ReadOperation**>(address)),
    _id(0) {

  if (address == nullptr || *address == nullptr) {
    _id = wal::LogfileManager::instance()->registerReadOperation(); 
    
    if (_id == 0) {
      throw "fail!"; // TODO: throw proper exception
    }

    if (_address != nullptr) {
      *address = this;
    }
  }
}

ReadOperation::ReadOperation () 
  : ReadOperation(nullptr) {
}  

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy a read operation
////////////////////////////////////////////////////////////////////////////////

ReadOperation::~ReadOperation () {
  if (_address == nullptr || *_address == this) {
    assert(_id != 0);

    wal::LogfileManager::instance()->unregisterReadOperation(_id);

    if (_address != nullptr) {
      *_address = nullptr;
    }
  }
}

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
#pragma once

#include <string_view>

namespace spl::logging
{
// Raw string: backslashes are literal. The single leading and trailing newline is stripped
// when printing. The art must start at column 0.
inline constexpr std::string_view kAsciiArt = R"(
                                                       .____                     .___            
_______   ____   __________  __ _________   ____  ____ |    |    _________     __| _/___________ 
\_  __ \_/ __ \ /  ___/  _ \|  |  \_  __ \_/ ___\/ __ \|    |   /  _ \__  \   / __ |/ __ \_  __ \
 |  | \/\  ___/ \___ (  <_> )  |  /|  | \/\  \__\  ___/|    |__(  <_> ) __ \_/ /_/ \  ___/|  | \/
 |__|    \___  >____  >____/|____/ |__|    \___  >___  >_______ \____(____  /\____ |\___  >__|   
             \/     \/                         \/    \/        \/         \/      \/    \/       
)";
} // namespace spl::logging

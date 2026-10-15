# Copyright 2026 https://github.com/ppetr/esp32s3-prometheus-exporter
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# third_party/ vendors uzlib's tinflate.c (Joergen Ibsen / Paul Sokolovsky,
# zlib license, see third_party/LICENSE.uzlib) unmodified: a raw-DEFLATE
# decoder with an incremental, bounded-output-buffer API
# (uzlib_uncompress()), unlike puff.c's single whole-buffer-only call -
# needed so self_doc (DESIGN.md §11.1) can decompress straight into an HTTP
# response in small chunks instead of buffering the whole document.
idf_component_register(
    SRCS "uzlib_inflate.c" "third_party/tinflate.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "third_party"
)

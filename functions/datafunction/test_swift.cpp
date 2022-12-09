#include <slsfs/storage-swiftkv-minio.hpp>
#include <slsfs/basetypes.hpp>
#include <string>
#include <iostream>


#include <filesystem>

using namespace std;


int main(int argc, char const *argv[])
{
   slsfs::storage::swiftkv swift_client = slsfs::storage::swiftkv("moc-kvstore");

   slsfs::pack::key_t key = slsfs::pack::key_t{
       7, 8, 7, 8, 7, 8, 7, 8,
       7, 8, 7, 8, 7, 8, 7, 8,
       7, 8, 7, 8, 7, 8, 7, 8,
       7, 8, 7, 8, 7, 8, 7, 8};

    std::string data = "hello swift1";

    swift_client.write_key(key, data.size(), slsfs::base::to_buf(data), 0, 0);
    cout << slsfs::base::to_string(swift_client.read_key(key, data.size(), 0, data.size())) << endl;
    // swift_client.get_list_key(key);
    
    return 0;
}

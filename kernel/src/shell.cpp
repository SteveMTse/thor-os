
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================

#include "keyboard.hpp"
#include "kernel_utils.hpp"
#include "console.hpp"
#include "shell.hpp"
#include "timer.hpp"
#include "memory.hpp"
#include "disks.hpp"
#include "ata.hpp"
#include "acpi.hpp"
#include "e820.hpp"
#include "rtc.hpp"
#include "elf.hpp"
#include "paging.hpp"
#include "gdt.hpp"
#include "vesa.hpp"

//Commands
#include "sysinfo.hpp"

#include "stl/types.hpp"
#include "stl/algorithms.hpp"
#include "stl/vector.hpp"
#include "stl/string.hpp"
#include "stl/optional.hpp"

namespace {

#ifdef CONFIG_HISTORY
static constexpr const bool History = true;
#else
static constexpr const bool History = false;
#endif

std::vector<std::string> history;
uint64_t history_index = 0;

bool shift = false;

//Declarations of the different functions

void reboot_command(const std::vector<std::string>& params);
void help_command(const std::vector<std::string>& params);
void uptime_command(const std::vector<std::string>& params);
void clear_command(const std::vector<std::string>& params);
void date_command(const std::vector<std::string>& params);
void sleep_command(const std::vector<std::string>& params);
void echo_command(const std::vector<std::string>& params);
void mmap_command(const std::vector<std::string>& params);
void memory_command(const std::vector<std::string>& params);
void memorydebug_command(const std::vector<std::string>& params);
void disks_command(const std::vector<std::string>& params);
void partitions_command(const std::vector<std::string>& params);
void mount_command(const std::vector<std::string>& params);
void unmount_command(const std::vector<std::string>& params);
void ls_command(const std::vector<std::string>& params);
void cd_command(const std::vector<std::string>& params);
void pwd_command(const std::vector<std::string>& params);
void free_command(const std::vector<std::string>& params);
void cat_command(const std::vector<std::string>& params);
void mkdir_command(const std::vector<std::string>& params);
void rm_command(const std::vector<std::string>& params);
void touch_command(const std::vector<std::string>& params);
void readelf_command(const std::vector<std::string>& params);
void exec_command(const std::vector<std::string>& params);
void execin_command(const std::vector<std::string>& params);
void shutdown_command(const std::vector<std::string>& params);
void vesainfo_command(const std::vector<std::string>& params);
void divzero_command(const std::vector<std::string>& params);

struct command_definition {
    const char* name;
    void (*function)(const std::vector<std::string>&);
};

command_definition commands[29] = {
    {"reboot", reboot_command},
    {"help", help_command},
    {"uptime", uptime_command},
    {"clear", clear_command},
    {"date", date_command},
    {"sleep", sleep_command},
    {"echo", echo_command},
    {"mmap", mmap_command},
    {"memory", memory_command},
    {"memorydebug", memorydebug_command},
    {"disks", disks_command},
    {"partitions", partitions_command},
    {"mount", mount_command},
    {"unmount", unmount_command},
    {"ls", ls_command},
    {"free", free_command},
    {"cd", cd_command},
    {"pwd", pwd_command},
    {"sysinfo", sysinfo_command},
    {"cat", cat_command},
    {"mkdir", mkdir_command},
    {"touch", touch_command},
    {"rm", rm_command},
    {"readelf", readelf_command},
    {"exec", exec_command},
    {"execin", execin_command},
    {"shutdown", shutdown_command},
    {"vesainfo", vesainfo_command},
    {"divzero", divzero_command},
};

std::string current_input(16);

void exec_shell_command();

template<bool Enable = History>
void history_key(char key){
    if(history.size() > 0){
        if(key == keyboard::KEY_UP){
            if(history_index == 0){
                return;
            }

            --history_index;
        } else { //KEY_DOWN
            if(history_index == history.size()){
                return;
            }

            ++history_index;
        }

        set_column(6);

        for(uint64_t i = 0; i < current_input.size(); ++i){
            k_print(' ');
        }

        set_column(6);

        if(history_index < history.size()){
            current_input = history[history_index];
        }

        k_print(current_input);
    }
}

template<> void history_key<false>(char){}

template<bool Enable = History>
void history_save(){
    history.push_back(current_input);
    history_index = history.size();
}

template<> void history_save<false>(){}

void start_shell(){
    while(true){
        auto key = keyboard::get_char();

        //Key released
        if(key & 0x80){
            key &= ~(0x80);
            if(key == keyboard::KEY_LEFT_SHIFT || key == keyboard::KEY_RIGHT_SHIFT){
                shift = false;
            }
        }
        //Key pressed
        else {
            //ENTER validate the command
            if(key == keyboard::KEY_ENTER){
                k_print_line();

                if(current_input.size() > 0){
                    exec_shell_command();

                    if(get_column() != 0){
                        k_print_line();
                    }

                    current_input.clear();
                }

                k_print("thor> ");
            } else if(key == keyboard::KEY_LEFT_SHIFT || key == keyboard::KEY_RIGHT_SHIFT){
                shift = true;
            } else if(key == keyboard::KEY_UP || key == keyboard::KEY_DOWN){
                history_key(key);
            } else if(key == keyboard::KEY_BACKSPACE){
                if(current_input.size() > 0){
                    k_print('\b');

                    current_input.pop_back();
                }
            } else {
                auto qwertz_key =
                    shift
                    ? keyboard::shift_key_to_ascii(key)
                    : keyboard::key_to_ascii(key);

                if(qwertz_key){
                    current_input += qwertz_key;

                    k_print(qwertz_key);
                }
            }
        }
    }
}

void exec_shell_command(){
    history_save();

    auto params = std::split(current_input);;

    for(auto& command : commands){
        if(params[0] == command.name){
            command.function(params);

            return;
        }
    }

    k_printf("The command \"%s\" does not exist\n", current_input.c_str());
}

void clear_command(const std::vector<std::string>&){
    wipeout();
}

void __attribute__((noreturn)) reboot_command(const std::vector<std::string>&){
    __asm__ __volatile__("mov al, 0x64; or al, 0xFE; out 0x64, al; mov al, 0xFE; out 0x64, al; " : : );

    __builtin_unreachable();
}

void help_command(const std::vector<std::string>&){
    k_print("Available commands:\n");

    for(auto& command : commands){
        k_print('\t');
        k_print_line(command.name);
    }
}

void uptime_command(const std::vector<std::string>&){
    k_printf("Uptime: %us\n", timer_seconds());
}

void date_command(const std::vector<std::string>&){
    auto data = rtc::all_data();

    k_printf("%u.%u.%u %u:%.2d:%.2d\n", data.day, data.month, data.year, data.hour, data.minute, data.second);
}

void sleep_command(const std::vector<std::string>& params){
    sleep_ms(parse(params[1]) * 1000);
}

void echo_command(const std::vector<std::string>& params){
    for(uint64_t i = 1; i < params.size(); ++i){
        k_print(params[i]);
        k_print(' ');
    }
    k_print_line();
}

void mmap_command(const std::vector<std::string>&){
    if(e820::mmap_failed()){
        k_print_line("The mmap was not correctly loaded from e820");
    } else {
        k_printf("There are %u mmap entry\n", e820::mmap_entry_count());

        k_print_line("Base         End          Size                  Type");
        for(uint64_t i = 0; i < e820::mmap_entry_count(); ++i){
            auto& entry = e820::mmap_entry(i);

            k_printf("%.10h %.10h %.10h %8m %s\n",
                entry.base, entry.base + entry.size, entry.size, entry.size, e820::str_e820_type(entry.type));
        }
    }
}

void memory_command(const std::vector<std::string>&){
    if(e820::mmap_failed()){
        k_print_line("The mmap was not correctly loaded from e820");
    } else {
        k_printf("Total available memory: %m\n", e820::available_memory());
        k_printf("Total used memory: %m\n", used_memory());
        k_printf("Total free memory: %m\n", free_memory());
        k_printf("Total allocated memory: %m\n", allocated_memory());
    }
}

void memorydebug_command(const std::vector<std::string>&){
    memory_debug();
}

void disks_command(const std::vector<std::string>& params){
    bool verbose = false;

    //Read options if any
    if(params.size() > 1){
        for(size_t i = 1; i < params.size(); ++i){
            if(params[i] == "-v"){
                verbose = true;
            }
        }
    }

    if(verbose){
        k_print_line("UUID       Type  Model                Serial          Firmware");
    } else {
        k_print_line("UUID       Type");
    }

    for(uint64_t i = 0; i < disks::detected_disks(); ++i){
        auto& descriptor = disks::disk_by_index(i);

        if(verbose){
            if(descriptor.type == disks::disk_type::ATA || descriptor.type == disks::disk_type::ATAPI){
                auto sub = static_cast<ata::drive_descriptor*>(descriptor.descriptor);

                k_printf("%10d %5s %20s %15s %s\n", descriptor.uuid, disks::disk_type_to_string(descriptor.type),
                    sub->model.c_str(), sub->serial.c_str(), sub->firmware.c_str());
            } else {
                k_printf("%10d %s\n", descriptor.uuid, disks::disk_type_to_string(descriptor.type));
            }
        } else {
            k_printf("%10d %s\n", descriptor.uuid, disks::disk_type_to_string(descriptor.type));
        }
    }
}

void partitions_command(const std::vector<std::string>& params){
    auto uuid = parse(params[1]);

    if(disks::disk_exists(uuid)){
        auto& disk = disks::disk_by_uuid(uuid);

        if(disk.type != disks::disk_type::ATA){
            k_print_line("Only ATA disks are supported");
        } else {
            auto partitions = disks::partitions(disk);

            if(partitions.size() > 0){
                k_print_line("UUID       Type         Start      Sectors");

                for(auto& partition : partitions){
                    k_printf("%10d %12s %10d %u\n", partition.uuid,
                        disks::partition_type_to_string(partition.type),
                        partition.start, partition.sectors);
                }
            }
        }
    } else {
        k_printf("Disks %u does not exist\n", uuid);
    }
}

void mount_command(const std::vector<std::string>& params){
    if(params.size() == 1){
        auto md = disks::mounted_disk();
        auto mp = disks::mounted_partition();

        if(md && mp){
            k_printf("%u:%u is mounted\n", md->uuid, mp->uuid);
        } else {
            k_print_line("Nothing is mounted");
        }
    } else {
        if(params.size() != 3){
            k_print_line("mount: Not enough params: mount disk partition");

            return;
        }

        auto disk_uuid = parse(params[1]);
        auto partition_uuid = parse(params[2]);

        if(disks::disk_exists(disk_uuid)){
            auto& disk = disks::disk_by_uuid(disk_uuid);

            if(disk.type != disks::disk_type::ATA){
                k_print_line("Only ATA disks are supported");
            } else {
                if(disks::partition_exists(disk, partition_uuid)){
                    disks::mount(disk, partition_uuid);
                } else {
                    k_printf("Partition %u does not exist\n", partition_uuid);
                }
            }
        } else {
            k_printf("Disk %u does not exist\n", disk_uuid);
        }
    }
}

void unmount_command(const std::vector<std::string>& ){
    if(!disks::mounted_partition() || !disks::mounted_disk()){
        k_print_line("Nothing is mounted");

        return;
    }

    disks::unmount();
}

void ls_command(const std::vector<std::string>& params){
    if(!disks::mounted_partition() || !disks::mounted_disk()){
        k_print_line("Nothing is mounted");

        return;
    }

    //By default hidden files are not shown
    bool show_hidden_files = false;
    bool list = false;

    //Read options if any
    if(params.size() > 1){
        for(size_t i = 1; i < params.size(); ++i){
            if(params[i] == "-a"){
                show_hidden_files = true;
            } else if(params[i] == "-l"){
                list = true;
            }
        }
    }

    auto files = disks::ls();
    size_t total = 0;

    for(auto& file : files){
        if(file.hidden && !show_hidden_files){
            continue;
        }

        ++total;

        if(list){
            if(file.directory){
                k_print(" d ");
            } else {
                k_print(" f ");
            }

            k_print(file.size);
            k_print(' ');

            k_print(file.created.day);
            k_print('.');
            k_print(file.created.month);
            k_print('.');
            k_print(1980+file.created.year);
            k_print(' ');

            k_print(file.created.hour);
            k_print(':');
            k_print(file.created.minutes);
            k_print(' ');

            k_print_line(file.file_name);
        } else {
            k_print(file.file_name);
            k_print(' ');
        }
    }

    if(!list){
        k_print('\n');
    }

    k_printf("Total: %u\n", total);
}

void free_command(const std::vector<std::string>&){
    if(!disks::mounted_partition() || !disks::mounted_disk()){
        k_print_line("Nothing is mounted");

        return;
    }

    k_printf("Free size: %m\n", disks::free_size());
}

void pwd_command(const std::vector<std::string>&){
    if(!disks::mounted_partition() || !disks::mounted_disk()){
        k_print_line("Nothing is mounted");

        return;
    }

    auto& cd = disks::current_directory();

    k_print('/');

    for(auto& p : cd){
        k_print(p);
        k_print('/');
    }

    k_print_line();
}

std::optional<disks::file> find_file(const std::string& name){
    auto files = disks::ls();

    for(auto& file : files){
        if(file.file_name == name){
            return {file};
        }
    }

    return {};
}

void cd_command(const std::vector<std::string>& params){
    if(!disks::mounted_partition() || !disks::mounted_disk()){
        k_print_line("Nothing is mounted");

        return;
    }

    //If there are no params, go to /
    if(params.size() == 1){
        disks::current_directory().clear();
    } else {
        if(params[1] == ".."){
            if(disks::current_directory().size() > 0){
                disks::current_directory().pop_back();
            }
        } else {
            auto file = find_file(params[1]);

            if(file){
                if(file->directory){
                    disks::current_directory().push_back(params[1]);
                } else {
                    k_print("cd: Not a directory: ");
                    k_print_line(params[1]);
                }
            } else {
                k_print("cd: No such file or directory: ");
                k_print_line(params[1]);
            }
        }
    }
}

void cat_command(const std::vector<std::string>& params){
    if(!disks::mounted_partition() || !disks::mounted_disk()){
        k_print_line("Nothing is mounted");

        return;
    }

    if(params.size() == 1){
        k_print_line("No file provided");
    } else {
        auto file = find_file(params[1]);

        if(file){
            if(!file->directory){
                auto content = disks::read_file(params[1]);
                k_print(content);
            } else {
                k_print("cd: Not a file: ");
                k_print_line(params[1]);
            }
        } else {
            k_print("cd: No such file or directory: ");
            k_print_line(params[1]);
        }
    }
}

void mkdir_command(const std::vector<std::string>& params){
    if(!disks::mounted_partition() || !disks::mounted_disk()){
        k_print_line("Nothing is mounted");

        return;
    }

    if(params.size() == 1){
        k_print_line("No directory provided");
    } else {
        auto& directory_name = params[1];
        auto directory = find_file(directory_name);

        if(directory){
            k_printf("mkdir: Cannot create directory '%s': File exists\n", directory_name.c_str());
        } else {
            if(!disks::mkdir(directory_name)){
                k_print_line("Directory creation failed");
            }
        }
    }
}

void touch_command(const std::vector<std::string>& params){
    if(!disks::mounted_partition() || !disks::mounted_disk()){
        k_print_line("Nothing is mounted");

        return;
    }

    if(params.size() == 1){
        k_print_line("No file name provided");
    } else {
        auto& file_name = params[1];
        auto file = find_file(file_name);

        if(file){
            k_printf("touch: Cannot create file '%s': File exists\n", file_name.c_str());
        } else {
            if(!disks::touch(file_name)){
                k_print_line("File creation failed");
            }
        }
    }
}

void rm_command(const std::vector<std::string>& params){
    if(!disks::mounted_partition() || !disks::mounted_disk()){
        k_print_line("Nothing is mounted");

        return;
    }

    if(params.size() == 1){
        k_print_line("No file name provided");
    } else {
        auto& file_name = params[1];
        auto file = find_file(file_name);

        if(!file){
            k_printf("rm: Cannot delete file '%s': No such file or directory\n", file_name.c_str());
        } else {
            if(!disks::rm(file_name)){
                k_print_line("File removal failed");
            }
        }
    }
}

void readelf_command(const std::vector<std::string>& params){
    if(params.size() < 2){
        k_print_line("readelf: Need the name of the executable to read");

        return;
    }

    if(!disks::mounted_partition() || !disks::mounted_disk()){
        k_print_line("Nothing is mounted");

        return;
    }

    auto content = disks::read_file(params[1]);

    if(content.empty()){
        k_print_line("readelf: The file does not exist or is empty");

        return;
    }

    if(!elf::is_valid(content)){
        k_print_line("readelf: This file is not an ELF file or not in ELF64 format");

        return;
    }

    auto buffer = content.c_str();
    auto header = reinterpret_cast<elf::elf_header*>(buffer);

    k_printf("Number of Program Headers: %u\n", static_cast<uint64_t>(header->e_phnum));
    k_printf("Number of Section Headers: %u\n", static_cast<uint64_t>(header->e_shnum));

    auto program_header_table = reinterpret_cast<elf::program_header*>(buffer + header->e_phoff);
    auto section_header_table = reinterpret_cast<elf::section_header*>(buffer + header->e_shoff);

    auto& string_table_header = section_header_table[header->e_shstrndx];
    auto string_table = buffer + string_table_header.sh_offset;

    for(size_t p = 0; p < header->e_phnum; ++p){
        auto& p_header = program_header_table[p];

        k_printf("Program header %u\n", p);
        k_printf("\tVirtual Address: %h\n", p_header.p_paddr);
        k_printf("\tMSize: %u\t", p_header.p_memsz);
        k_printf("\tFSize: %u\t Offset: %u \n", p_header.p_filesize, p_header.p_offset);
    }

    for(size_t s = 0; s < header->e_shnum; ++s){
        auto& s_header = section_header_table[s];

        k_printf("Section \"%s\" (", &string_table[s_header.sh_name]);

        if(s_header.sh_flags & 0x1){
            k_print(" W");
        }

        if(s_header.sh_flags & 0x2){
            k_print(" A");
        }

        if(s_header.sh_flags & 0x4){
            k_print(" X");
        }

        if(s_header.sh_flags & 0x0F000000){
            k_print(" OS");
        }

        if(s_header.sh_flags & 0xF0000000){
            k_print(" CPU");
        }
        k_print_line(")");
        k_printf("\tAddress: %h Size: %u Offset: %u\n", s_header.sh_addr, s_header.sh_size, s_header.sh_offset);
    }
}

std::optional<std::string> read_elf_file(const std::string& file, const std::string& name){
    auto content = disks::read_file(file);

    if(content.empty()){
        k_print(name);
        k_print_line(": The file does not exist or is empty");

        return {};
    }

    if(!elf::is_valid(content)){
        k_print(name);
        k_print_line(": This file is not an ELF file or not in ELF64 format");

        return {};
    }

    return {std::move(content)};
}

bool allocate_segments(char* buffer, void** allocated_segments, uint8_t flags){
    auto header = reinterpret_cast<elf::elf_header*>(buffer);
    auto program_header_table = reinterpret_cast<elf::program_header*>(buffer + header->e_phoff);

    bool failed = false;
    for(size_t p = 0; p < header->e_phnum; ++p){
        auto& p_header = program_header_table[p];

        allocated_segments[p] = nullptr;

        if(p_header.p_type == 1){
            //0. Calculate some stuff
            auto address = p_header.p_vaddr;
            auto first_page = reinterpret_cast<uintptr_t>(paging::page_align(reinterpret_cast<void*>(address)));
            auto left_padding = address - first_page;
            auto bytes = left_padding + paging::PAGE_SIZE + p_header.p_memsz;
            auto pages = (bytes / paging::PAGE_SIZE) + 1;

            //1. Verify that all the necessary pages are free
            for(size_t i = 0; i < pages; ++i){
                if(paging::page_present(reinterpret_cast<void*>(first_page + i * paging::PAGE_SIZE))){
                    failed = true;
                    break;
                }
            }

            if(failed){
                k_print_line("Some pages are already mapped");
                break;
            }

            //2. Get enough physical memory
            auto memory = k_malloc(bytes);

            if(!memory){
                k_print_line("Cannot allocate memory, probably out of memory");
                failed = true;
                break;
            }

            //Save it to be able to free it later
            allocated_segments[p] = memory;

            //3. Find a start of a page inside the physical memory

            auto aligned_memory = paging::page_aligned(memory) ? memory :
                reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(memory) / paging::PAGE_SIZE + 1) * paging::PAGE_SIZE);

            //4. Map physical allocated memory to the necessary virtual memory

            if(!paging::map_pages(reinterpret_cast<void*>(first_page), aligned_memory, pages, flags)){
                k_print_line("Mapping the pages failed");
                failed = true;
                break;
            }

            //5. Copy memory

            auto memory_start = reinterpret_cast<uintptr_t>(aligned_memory) + left_padding;

            std::copy_n(reinterpret_cast<char*>(memory_start), buffer + p_header.p_offset, p_header.p_memsz);
        }
    }

    return failed;
}

void* allocate_user_stack(size_t stack_address, size_t stack_size, uint8_t flags){
    //0. Calculate some stuff
    auto address = stack_address;
    auto first_page = reinterpret_cast<uintptr_t>(paging::page_align(reinterpret_cast<void*>(address)));
    auto left_padding = address - first_page;
    auto bytes = left_padding + paging::PAGE_SIZE + stack_size;
    auto pages = (bytes / paging::PAGE_SIZE) + 1;

    //1. Verify that all the necessary pages are free
    for(size_t i = 0; i < pages; ++i){
        if(paging::page_present(reinterpret_cast<void*>(first_page + i * paging::PAGE_SIZE))){
            k_print_line("Some pages are already mapped");
            return nullptr;
        }
    }

    //2. Get enough physical memory
    auto memory = k_malloc(bytes);

    if(!memory){
        k_print_line("Cannot allocate memory, probably out of memory");
        return nullptr;
    }

    //3. Find a start of a page inside the physical memory

    auto aligned_memory = paging::page_aligned(memory) ? memory :
        reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(memory) / paging::PAGE_SIZE + 1) * paging::PAGE_SIZE);

    //4. Map physical allocated memory to the necessary virtual memory

    if(!paging::map_pages(reinterpret_cast<void*>(first_page), aligned_memory, pages, flags)){
        k_print_line("Mapping the pages failed");
        return nullptr;
    }

    //5. Zero-out memory

    auto memory_start = reinterpret_cast<uintptr_t>(aligned_memory) + left_padding;

    std::fill_n(reinterpret_cast<char*>(memory_start), stack_size, 0);

    return memory;
}

void release_segments(char* buffer, void** allocated_segments){
    auto header = reinterpret_cast<elf::elf_header*>(buffer);
    auto program_header_table = reinterpret_cast<elf::program_header*>(buffer + header->e_phoff);

    //Release physical memory
    for(size_t p = 0; p < header->e_phnum; ++p){
        auto& p_header = program_header_table[p];

        auto a = allocated_segments[p];
        if(a){
            k_free(a);

            auto address = p_header.p_vaddr;
            auto first_page = reinterpret_cast<uintptr_t>(paging::page_align(reinterpret_cast<void*>(address)));
            auto left_padding = address - first_page;
            auto bytes = left_padding + paging::PAGE_SIZE + p_header.p_memsz;
            auto pages = (bytes / paging::PAGE_SIZE) + 1;

            if(!paging::unmap_pages(reinterpret_cast<void*>(first_page), pages)){
                k_print_line("Unmap failed, memory could be in invalid state");
            }
        }
    }
}

void exec_command(const std::vector<std::string>& params){
    if(params.size() < 2){
        k_print_line("exec: Need the name of the executable to read");

        return;
    }

    if(!disks::mounted_partition() || !disks::mounted_disk()){
        k_print_line("Nothing is mounted");

        return;
    }

    auto content = read_elf_file(params[1], "exec");

    if(!content){
        return;
    }

    auto buffer = content->c_str();
    auto header = reinterpret_cast<elf::elf_header*>(buffer);

    std::unique_heap_array<void*> allocated_segments(header->e_phnum);

    auto failed = allocate_segments(buffer, allocated_segments.get(), paging::PRESENT | paging::WRITE | paging::USER);

    if(!failed){
        auto stack_physical = allocate_user_stack(0x500000, paging::PAGE_SIZE * 2, paging::PRESENT | paging::WRITE | paging::USER);

        if(stack_physical){
            uint64_t rsp;
            asm volatile("mov %0, rsp;" : "=m" (rsp));
            gdt::tss.rsp0 = rsp;

            asm volatile("mov ax, %0; mov ds, ax; mov es, ax; mov fs, ax; mov gs, ax;"
                :  //No outputs
                : "i" (gdt::USER_DATA_SELECTOR + 3)
                : "rax");

            asm volatile("push %0; push %1; pushfq; push %2; push %3; iretq"
                :  //No outputs
                : "i" (gdt::USER_DATA_SELECTOR + 3), "i" (0x500000 + paging::PAGE_SIZE * 2 - 64), "i" (gdt::USER_CODE_SELECTOR + 3), "r" (header->e_entry)
                : "rax");

            //TODO Release stack
        } else {
            k_print_line("Unable to allocate a stack for the program");
        }
    } else {
        k_print_line("execin: Unable to execute the program");
    }

    release_segments(buffer, allocated_segments.get());
}

void execin_command(const std::vector<std::string>& params){
    if(params.size() < 2){
        k_print_line("execin: Need the name of the executable to read");

        return;
    }

    if(!disks::mounted_partition() || !disks::mounted_disk()){
        k_print_line("Nothing is mounted");

        return;
    }

    auto content = read_elf_file(params[1], "execin");

    if(!content){
        return;
    }

    auto buffer = content->c_str();
    auto header = reinterpret_cast<elf::elf_header*>(buffer);

    std::unique_heap_array<void*> allocated_segments(header->e_phnum);

    auto failed = allocate_segments(buffer, allocated_segments.get(), paging::PRESENT | paging::WRITE);

    if(!failed){
        auto main_function = reinterpret_cast<int(*)()>(header->e_entry);

        auto return_code = main_function();

        k_printf("Returned %d\n", return_code);
    } else {
        k_print_line("execin: Unable to execute the program");
    }

    release_segments(buffer, allocated_segments.get());
}

void vesainfo_command(const std::vector<std::string>&){
    if(vesa::vesa_enabled){
        auto& block = vesa::mode_info_block;

        k_print_line("VESA Enabled");
        k_printf("Resolution: %ux%u\n", static_cast<size_t>(block.width), static_cast<size_t>(block.height));
        k_printf("Depth: %u\n", static_cast<size_t>(block.bpp));
        k_printf("Pitch: %u\n", static_cast<size_t>(block.pitch));
        k_printf("LFB Address: %h\n", static_cast<size_t>(block.linear_video_buffer));
        k_printf("Offscreen Memory Size: %h\n", static_cast<size_t>(block.offscreen_memory_size));
        k_printf("Maximum Pixel Clock: %h\n", static_cast<size_t>(block.maximum_pixel_clock));

        k_printf("Red Mask Size: %u\n", static_cast<size_t>(block.linear_red_mask_size));
        k_printf("Red Mask Position: %u\n", static_cast<size_t>(block.linear_red_mask_position));
        k_printf("Green Mask Size: %u\n", static_cast<size_t>(block.linear_green_mask_size));
        k_printf("Green Mask Position: %u\n", static_cast<size_t>(block.linear_green_mask_position));
        k_printf("Blue Mask Size: %u\n", static_cast<size_t>(block.linear_blue_mask_size));
        k_printf("Blue Mask Position: %u\n", static_cast<size_t>(block.linear_blue_mask_position));
    } else {
        k_print_line("VESA Disabled");
    }
}

void shutdown_command(const std::vector<std::string>&){
    if(!acpi::init()){
        k_print_line("Unable to init ACPI");
    }

    acpi::shutdown();
}

void divzero_command(const std::vector<std::string>&){
    asm volatile("xor ebx, ebx; div ebx;" : : : "memory");
}

} //end of anonymous namespace

void init_shell(){
    wipeout();

    k_print("thor> ");

    start_shell();
}

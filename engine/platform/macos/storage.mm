#import <Foundation/Foundation.h>
#include "domain/session.hpp"

namespace daw {
std::string replacementDirectory(const std::string& target) {
    @autoreleasepool {
        NSString* path = [[NSString alloc] initWithBytes:target.data() length:target.size() encoding:NSUTF8StringEncoding];
        if (!path) throw Error("Invalid destination path");
        NSError* error = nil;
        NSURL* directory = [[NSFileManager defaultManager] URLForDirectory:NSItemReplacementDirectory
            inDomain:NSUserDomainMask appropriateForURL:[NSURL fileURLWithPath:path] create:YES error:&error];
        if (!directory) throw Error("Cannot create system replacement directory");
        const char* result = directory.fileSystemRepresentation;
        if (!result) throw Error("Invalid replacement directory path");
        return result;
    }
}
}

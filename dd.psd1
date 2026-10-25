@{
    schema = 1
    project = @{
        name = 'lib8bit'
        type = 'gui'
        'default-target' = 'app'
    }
    dependencies = @{ owner = 'dd' }
    build = @{
        'x64-windows' = @{
            debug = 'debug'
            release = 'release'
        }
    }
    targets = @(
        @{
            id = 'lib'
            kind = 'library'
            'cmake-target' = 'lib8bit'
            'test-label' = 'lib8bit'
            'debug-path' = 'build/debug/{libprefix}lib8bit{lib}'
            'release-path' = 'build/release/{libprefix}lib8bit{lib}'
            platforms = @('x64-windows')
        },
        @{
            id = 'app'
            kind = 'gui'
            'cmake-target' = 'app8bit'
            'test-label' = 'lib8bit'
            'debug-path' = 'bin/app8bit64d{exe}'
            'release-path' = 'bin/app8bit64r{exe}'
            platforms = @('x64-windows')
        }
    )
}

param(
    [string]$Name = "hpm6e00evk",
    [string]$Opts = ""
)

Set-Location "$PSScriptRoot\..\.."

switch ($Name) {
    "board_rm_c" {
        west build -b stm32f4_disco $Opts -- -DBOARD_CFG=$Name
    }
    default {
        west build -b $Name $Opts
    }
}

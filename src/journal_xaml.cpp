// SPDX-License-Identifier: GPL-3.0-only
#include "journal.hpp"

#include <windows.h>
#include <algorithm>
#include <ctime>

namespace ach {
namespace {
constexpr auto Trophy = "/ForzaUI;component/Textures/Anthem/HUD/MinorNotificationIcons/Notification_Achievement.png";
constexpr auto Lock = "/ForzaUI;component/Textures/Anthem/Icons/PauseMenu/Lock.png";
constexpr const char* Sections[JournalSections] = {
    "RECENTLY UNLOCKED", "FESTIVAL & CAREER", "RACING & BLUEPRINTS", "CARS & GARAGE",
    "EXPLORATION & PR STUNTS", "SKILLS & LEVELS", "ONLINE", "BLIZZARD MOUNTAIN", "HOT WHEELS",
    "HOONIGAN", "#FORZATHON"
};

int sectionFor(const Achievement& achievement) {
    if (achievement.group != Group::base) return 6 + static_cast<int>(achievement.group);
    if (achievement.online) return 6;
    switch (achievement.id) {
    case 3: case 10: case 11: case 12: case 14: case 17: case 26: case 27: case 42:
    case 50: case 51: case 52: case 53: case 56: case 61: return 2;
    case 9: case 15: case 16: case 33: case 41: case 43: case 46: case 54: case 55: return 3;
    case 4: case 5: case 28: case 29: case 30: case 31: case 44: case 57: case 58:
    case 59: case 60: case 62: case 64: return 4;
    case 32: case 34: case 48: case 49: case 63: return 5;
    default: return 1;
    }
}

std::string escaped(std::string_view value) {
    std::string result;
    for (const char c : value) {
        switch (c) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        default: result += c; break;
        }
    }
    return result;
}

std::string utf8(const wchar_t* text) {
    const auto count = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), count, nullptr, nullptr);
    result.pop_back();
    return result;
}

constexpr std::string_view Header = R"xaml(<Page xmlns:local="clr-namespace:ForzaUI"
  xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
  xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
  xmlns:System="clr-namespace:System;assembly=mscorlib"
  x:Class ="ForzaUI.A_HudOptions"
  Style="{DynamicResource A_Style_Page_Generic}"
  FocusManager.FocusedElement="{Binding ElementName=OptionsList}"
  DataContext="{Binding DataSource, RelativeSource={RelativeSource Self}}">
  <local:UIPageTransitionManager.TransitionManager>
    <local:UIPageTransitionManager Preset="{StaticResource A_UIPageTransitionManagerPreset_Menu}"/>
  </local:UIPageTransitionManager.TransitionManager>
  <Page.Resources>
    <ResourceDictionary>
      <System:Double x:Key="A_Double_HUDSafeFrameHorizontalMax">0.69</System:Double>
      <System:Double x:Key="A_Double_HUDSafeFrameVerticalMax">0.25</System:Double>
      <local:FZBooleanToVisibilityConverter x:Key="AchVisible" UseHidden="true" InvertResult="false" />
      <Style x:Key="AchSection" TargetType="{x:Type ListBoxItem}" BasedOn="{StaticResource A_Style_ListBoxItem_List_Default}">
        <Setter Property="Height" Value="54" />
      </Style>
      <Style x:Key="AchCard" TargetType="{x:Type ListBoxItem}" BasedOn="{StaticResource A_Style_ListBoxItem_List_Default}">
        <Setter Property="Height" Value="112" />
      </Style>
      <ControlTemplate x:Key="AchPlain" TargetType="{x:Type Button}"><ContentPresenter /></ControlTemplate>
      <Style x:Key="AchText" TargetType="local:FZTextBlock" BasedOn="{StaticResource A_Style_FZTextBlock_AnthemA}">
        <Setter Property="Color" Value="#FF21293A" />
      </Style>
    </ResourceDictionary>
  </Page.Resources>
  <Grid>
    <local:FZMaterial Style="{StaticResource A_Style_PauseMenuBlur}" />
    <local:PlayerBarControl x:Name="TopBar" Style="{StaticResource A_Style_PlayerBarControl}" />
    <ContentControl x:Name="BottomBar" Style="{StaticResource A_Style_ButtonBar}">
      <ContentControl x:Name="DefaultHelpPanel"
        local:ControllerButtonPanel.ADescription="{local:Loc HelpButtons.IDS_Select}"
        local:ControllerButtonPanel.BDescription="{local:Loc HelpButtons.IDS_Back}"
        local:ControllerButtonPanel.LBumperDescription="{local:Loc HelpButtons.IDS_Previous}"
        local:ControllerButtonPanel.RBumperDescription="{local:Loc HelpButtons.IDS_Next}"
        Style="{StaticResource A_Style_ContentControl_DefaultButtonBar}" />
    </ContentControl>
    <Border Style="{StaticResource A_Style_Border_ScenePadding}">
      <Grid x:Name="LayoutRoot">
        <Grid Style="{StaticResource A_Style_FrameworkElement_ScreenTilt}" Focusable="False">
          <StackPanel Orientation="Vertical" HorizontalAlignment="Center" VerticalAlignment="Center">
)xaml";

std::string bar(int width, int height, int64_t current, int64_t target) {
    const auto fill = target > 0 ? std::clamp<int64_t>(width * current / target, 0, width) : 0;
    std::string out = "<Grid Width=\"" + std::to_string(width) + "\" Height=\"" + std::to_string(height) +
                      "\" HorizontalAlignment=\"Left\" Margin=\"0,4,0,0\">"
                      "<local:FZMaterial Color=\"#FFCECFD4\" />";
    if (fill)
        out += "<local:FZMaterial Color=\"#FFFF651E\" Width=\"" + std::to_string(fill) +
               "\" HorizontalAlignment=\"Left\" />";
    return out + "</Grid>\n";
}

std::string label(const std::string& value, int size, std::string_view extra = {}) {
    return "<local:FZTextBlock Text=\"" + escaped(value) + "\" FontSize=\"" + std::to_string(size) +
           "\" Style=\"{StaticResource AchText}\" " + std::string(extra) + " />";
}

std::string openedDate(int64_t stamp) {
    const auto time = static_cast<std::time_t>(stamp);
    tm date{};
    char text[16]{};
    if (gmtime_s(&date, &time) == 0) std::strftime(text, sizeof text, "%Y-%m-%d", &date);
    return text[0] ? "UNLOCKED " + std::string(text) : "UNLOCKED";
}

std::string countText(int got, size_t all) {
    return std::to_string(got) + " / " + std::to_string(all);
}

std::string list(const std::string& name, int width, int height, const char* rowStyle,
                 std::string_view extra = {}) {
    return "<local:CommonListBox x:Name=\"" + name +
           "\" Style=\"{StaticResource A_Style_ListBox_List_Default_ScrollBar}\""
           " KeyboardNavigation.DirectionalNavigation=\"Contained\" ItemContainerStyle=\"{StaticResource " +
           rowStyle + "}\" Width=\"" + std::to_string(width) + "\" Height=\"" + std::to_string(height) +
           "\" " + std::string(extra) + ">\n";
}

std::string card(const Achievement& entry, const Progress& p, int64_t stamp, int section, size_t index) {
    const auto status = stamp ? openedDate(stamp) : entry.online ? std::string("ONLINE ONLY") :
                        p.target ? std::to_string(p.current) + " / " + std::to_string(p.target) : "UNTRACKED";
    std::string x = "<Button x:Name=\"AchCard_" + std::to_string(section) + "_" + std::to_string(index) +
                    "\" Template=\"{StaticResource AchPlain}\"><Grid Height=\"110\" Margin=\"4,0,12,0\">"
                    "<Grid.ColumnDefinitions><ColumnDefinition Width=\"62\" /><ColumnDefinition />"
                    "<ColumnDefinition Width=\"75\" /></Grid.ColumnDefinitions>";
    x += "<local:FZMaterial Width=\"37\" Height=\"37\" HorizontalAlignment=\"Left\" VerticalAlignment=\"Center\""
         " Color=\"" + std::string(stamp ? "#FFFF651E" : "#FF777D86") + "\" ImageSource=\"" +
         (stamp ? Trophy : Lock) + "\" />";
    x += "<StackPanel Grid.Column=\"1\" VerticalAlignment=\"Center\">";
    x += label(utf8(entry.title), 24);
    x += label(utf8(entry.condition), 18, "TextWrapping=\"Wrap\" Margin=\"0,1,0,1\"");
    x += "<StackPanel Orientation=\"Horizontal\" Margin=\"0,4,0,0\">";
    if (!stamp && p.target) x += bar(178, 5, p.current, p.target);
    x += label(status, 16, "Margin=\"8,0,0,0\"");
    x += "</StackPanel></StackPanel>";
    x += label(std::to_string(entry.gamerscore) + "G", 24,
               "Grid.Column=\"2\" HorizontalAlignment=\"Right\" VerticalAlignment=\"Center\"");
    return x + "</Grid></Button>\n";
}
}

std::string journalXaml(const ProgressList& progress, const Unlocks& unlocks) {
    constexpr int Left = 426, Right = 814, Total = Left + Right + 8, Height = 620;
    std::array<std::vector<const Achievement*>, JournalSections> cards;
    int score = 0, got = 0;
    for (const auto& entry : Catalog) {
        cards[sectionFor(entry)].push_back(&entry);
        if (unlocks[entry.id]) {
            cards[0].push_back(&entry);
            score += entry.gamerscore;
            ++got;
        }
    }
    std::stable_sort(cards[0].begin(), cards[0].end(),
                     [&](auto* a, auto* b) { return unlocks[a->id] > unlocks[b->id]; });
    if (cards[0].size() > 8) cards[0].resize(8);
    for (size_t i = 1; i < cards.size(); ++i) {
        std::stable_sort(cards[i].begin(), cards[i].end(), [&](auto* a, auto* b) {
            auto order = [&](const Achievement* item) {
                const auto& p = progress[item->id];
                if (unlocks[item->id]) return 0;
                if (p.target) return 1;
                return item->online ? 2 : 3;
            };
            const auto oa = order(a), ob = order(b);
            if (oa != ob) return oa < ob;
            const auto& pa = progress[a->id];
            const auto& pb = progress[b->id];
            return oa == 1 && pa.current * pb.target > pb.current * pa.target;
        });
    }

    std::string x(Header);
    x += "<Grid Width=\"" + std::to_string(Total) + "\" Margin=\"0,0,0,6\""
         " Style=\"{StaticResource A_Style_FrameworkElement_DefaultTransition_0}\">";
    x += "<ContentControl Content=\"ACHIEVEMENTS\" Focusable=\"False\" HorizontalAlignment=\"Left\""
         " Style=\"{StaticResource A_Style_ContentControl_ScreenTitleBlock}\" local:BindingProxy.A=\"";
    x += Trophy;
    x += "\" />";
    x += "<local:FZMaterial Color=\"#FF21293A\" Width=\"360\" Height=\"52\" HorizontalAlignment=\"Right\">";
    x += "<local:FZTextBlock Color=\"White\" FontSize=\"27\" Text=\"" + countText(got, std::size(Catalog)) +
         "     " + std::to_string(score) + " / 3000G\""
         " HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\" /></local:FZMaterial></Grid>\n";
    x += "<StackPanel Orientation=\"Horizontal\" Style=\"{StaticResource A_Style_FrameworkElement_DefaultTransition_0}\">\n";
    x += "<local:FZMaterial Color=\"#FFF5F5F5\" Margin=\"0,0,8,0\" local:UIPageTransitionManager.TransitionIndex=\"0\">";
    x += list("OptionsList", Left, Height, "AchSection",
              "SelectedIndex=\"{Binding FH3AchIndex, Mode=TwoWay}\" IsEnabled=\"{Binding FH3AchSections}\"");
    for (int i = 0; i < JournalSections; ++i) {
        const int count = static_cast<int>(std::ranges::count_if(cards[i], [&](auto* d) { return unlocks[d->id] != 0; }));
        const std::string name = i ? "AchSection_" + std::to_string(i) : "FirstItem";
        x += "<Button x:Name=\"" + name + "\" Template=\"{StaticResource AchPlain}\"><Grid Height=\"51\" Margin=\"0,0,10,0\">";
        x += "<local:FZMaterial Width=\"4\" HorizontalAlignment=\"Left\" Color=\"#FFFF651E\" Visibility=\"{Binding FH3AchGroup" +
             std::to_string(i) + ", Converter={StaticResource AchVisible}}\" />";
        x += label(Sections[i], 22, "Margin=\"12,0,62,0\" VerticalAlignment=\"Center\"");
        x += label(i ? countText(count, cards[i].size()) : std::to_string(count), 18,
                   "HorizontalAlignment=\"Right\" VerticalAlignment=\"Center\"");
        x += "</Grid></Button>\n";
    }
    x += "</local:CommonListBox></local:FZMaterial>\n<Grid local:UIPageTransitionManager.TransitionIndex=\"1\">";
    for (int i = 0; i < JournalSections; ++i) {
        const int count = static_cast<int>(std::ranges::count_if(cards[i], [&](auto* d) { return unlocks[d->id] != 0; }));
        x += "<StackPanel Visibility=\"{Binding FH3AchGroup" + std::to_string(i) +
             ", Converter={StaticResource AchVisible}}\">\n";
        x += "<local:FZMaterial Color=\"#FF21293A\" Height=\"58\" Width=\"" + std::to_string(Right) +
             "\"><Grid Margin=\"22,0,22,0\"><local:FZTextBlock Color=\"White\" FontSize=\"25\" Text=\"" + escaped(Sections[i]) +
             "\" VerticalAlignment=\"Center\" /><local:FZTextBlock Color=\"#FFC6C7CB\" FontSize=\"20\" Text=\"" +
             (i ? countText(count, cards[i].size()) : "LAST 8 UNLOCKS") +
             "\" VerticalAlignment=\"Center\" HorizontalAlignment=\"Right\" /></Grid></local:FZMaterial>";
        x += "<local:FZMaterial Color=\"#FFF5F5F5\">";
        x += list("AchList_" + std::to_string(i), Right, Height - 58, "AchCard");
        if (cards[i].empty()) {
            x += "<Button x:Name=\"AchCard_" + std::to_string(i) + "_0\" Template=\"{StaticResource AchPlain}\">"
                 "<StackPanel Margin=\"22,20,22,0\">" + label("Nothing unlocked yet", 24) +
                 label("Your most recent achievements will appear here.", 17) + "</StackPanel></Button>";
        }
        for (size_t j = 0; j < cards[i].size(); ++j) {
            const auto& entry = *cards[i][j];
            x += card(entry, progress[entry.id], unlocks[entry.id], i, j);
        }
        x += "</local:CommonListBox></local:FZMaterial></StackPanel>\n";
    }
    return x + "</Grid></StackPanel></StackPanel></Grid></Grid></Border></Grid></Page>\n";
}
}

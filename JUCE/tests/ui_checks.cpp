#include "MainComponent.h"
#include <iostream>
#include <stdexcept>

int main()
{
    juce::ScopedJuceInitialiser_GUI initialise;
    MainComponent main;
    main.setVisible(true);
    juce::TabbedComponent* tabs = nullptr;
    for (auto* child : main.getChildren())
        if (auto* candidate = dynamic_cast<juce::TabbedComponent*>(child)) tabs = candidate;
    if (tabs == nullptr) throw std::runtime_error("Missing tabs");
    tabs->setCurrentTabIndex(3);
    auto* panel = tabs->getTabContentComponent(3);
    int visible = 0;
    for (auto* child : panel->getChildren())
    {
        if (child->isVisible() && child->getWidth() > 0 && child->getHeight() > 0) ++visible;
        else throw std::runtime_error("Generator child has empty layout");
    }
    if (visible < 10) throw std::runtime_error("Generator controls missing");
    juce::PNGImageFormat png;
    {
        juce::FileOutputStream output(juce::File("/tmp/opensmaart-generator.png"));
        png.writeImageToStream(main.createComponentSnapshot(main.getLocalBounds()), output);
    }
    tabs->setCurrentTabIndex(0);
    {
        juce::FileOutputStream output(juce::File("/tmp/opensmaart-rta.png"));
        png.writeImageToStream(main.createComponentSnapshot(main.getLocalBounds()), output);
    }
    std::cout << "PASS: Generator tab has " << visible << " visible controls without resizing; screenshots saved\n";
}
